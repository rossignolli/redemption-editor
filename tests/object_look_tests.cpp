// Object look track (custom texture and tint): the tint bits, what a read
// stores (no track for the default look, the seeded default before a later
// look, changes only, never back in time), the look at the playhead, the TXDs
// a look streams, the detector's comparison, and the stand-in to prop handoff
// overlap (object_check.h HandoffPropReady).
#include "object_check.h"
#include "object_look_core.h"
#include "track_index.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay;

    int g_lookFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_lookFailures;
    }

    constexpr std::uint32_t kCatfish = 0x5A0D6A3Bu; // any non-zero hash

    ObjectLook Look(std::uint32_t txd, std::uint8_t tint = 0)
    {
        ObjectLook look;
        look.txd = txd;
        look.tint = tint;
        return look;
    }

    void TestTintBits()
    {
        // The writer 0xE5057C keeps bits 3..8 of +0xB8: and 0xFFFFFE07, or (tint & 0x3F) << 3.
        const std::uint32_t word = 0xFFFFFE07u | (13u << 3);
        Check(TintFromWord(word) == 13, "tint: bits 3..8 of the word");
        Check(TintFromWord(0x7u) == 0 && TintFromWord(0x1F8u) == 63, "tint: the other bits ignored, six bits wide");
        ObjectLook look = Look(0, 0xFF);
        SanitizeObjectLook(look);
        Check(look.tint == 63, "tint: a loaded value bounded to six bits");
        Check(kObjectCustomTxdOffset == 0x420 && kEntityTintWordOffset == 0xB8, "offsets: the setter's and the writer's (dump 1491.50)");
    }

    void TestNote()
    {
        std::vector<ObjectLookTrack> tracks;
        TrackIndex<ObjectLookTrack> index;
        bool created = false;
        Check(!NoteObjectLook(tracks, index, 7, 0, false, 0, ObjectLook{}, &created) && tracks.empty() && !created,
            "note: the default look makes no track");
        Check(NoteObjectLook(tracks, index, 7, 0, false, 0, Look(kCatfish), &created) && created && tracks.size() == 1 &&
                  tracks[0].states.size() == 1 && tracks[0].states[0].second.txd == kCatfish,
            "note: a first read with a texture makes the track (no seeded default)");
        Check(!NoteObjectLook(tracks, index, 7, 500, true, 0, Look(kCatfish)) && tracks[0].states.size() == 1,
            "note: the same look again stores nothing");
        Check(NoteObjectLook(tracks, index, 7, 1000, true, 500, Look(kCatfish, 4)) && tracks[0].states.size() == 2 &&
                  tracks[0].states[1].second.tint == 4,
            "note: a tint change is stored");
        Check(!NoteObjectLook(tracks, index, 7, 900, true, 1000, Look(0)) && tracks[0].states.size() == 2,
            "note: never back in time");
        Check(NoteObjectLook(tracks, index, 7, 1500, true, 1000, ObjectLook{}) && tracks[0].states.back().second == ObjectLook{},
            "note: back to the default is a change once there is a track");
        // An object read with the default before, a letter put on it later.
        Check(NoteObjectLook(tracks, index, 9, 2000, true, 1500, Look(kCatfish), &created) && created && tracks.size() == 2 &&
                  tracks[1].states.size() == 2 && tracks[1].states[0].first == 1500 && tracks[1].states[0].second == ObjectLook{} &&
                  tracks[1].states[1].first == 2000,
            "note: a later look is preceded by the default at the previous read");
    }

    void TestLookAt()
    {
        ObjectLookTrack track;
        track.id = 1;
        track.states = {{1000u, Look(kCatfish)}, {3000u, Look(kCatfish, 2)}};
        Check(ObjectLookAt(track, 500, false) == ObjectLook{}, "look at: before the first state, the default for a puppet");
        Check(ObjectLookAt(track, 500, true) == Look(kCatfish), "look at: before the first state, held for a stand-in");
        Check(ObjectLookAt(track, 1000, false) == Look(kCatfish) && ObjectLookAt(track, 2999, false) == Look(kCatfish),
            "look at: the state in force");
        Check(ObjectLookAt(track, 9000, false) == Look(kCatfish, 2), "look at: the last state after the end");
        ObjectLookTrack empty;
        Check(ObjectLookAt(empty, 100, true) == ObjectLook{}, "look at: an empty track is the default");
    }

    void TestTxds()
    {
        std::uint32_t out[3]{};
        ObjectLook look;
        Check(ObjectLookTxds(look, out) == 0, "txds: none for the default");
        look.txd = 5;
        look.p2 = 5;
        look.p3 = 9;
        Check(ObjectLookTxds(look, out) == 2 && out[0] == 5 && out[1] == 9, "txds: non-zero and distinct");
        Check(ObjectTexturesDiffer(Look(5), Look(5, 3)) == false && ObjectTexturesDiffer(Look(5), Look(6)),
            "textures differ: the tint aside");
    }

    void TestDetector()
    {
        Check(CheckObjectLook(Look(kCatfish, 1), Look(kCatfish, 1)) == LookCheck::Match, "detector: the same look matches");
        Check(CheckObjectLook(Look(kCatfish), Look(0)) == LookCheck::TextureMismatch &&
                  std::string(LookCheckText(LookCheck::TextureMismatch)) == "texture",
            "detector: the white note (texture missing)");
        Check(CheckObjectLook(Look(0, 3), Look(0, 0)) == LookCheck::TintMismatch, "detector: the tint missing");
        Check(CheckObjectLook(Look(kCatfish, 3), Look(0, 0)) == LookCheck::BothMismatch, "detector: both missing");
        Check(std::string(objcheck::VerdictText(objcheck::Verdict::LookMismatch)) == "object_look_mismatch",
            "detector: the verdict name qa_health counts");
    }

    void TestHandoff()
    {
        using objcheck::HandoffPropReady;
        // Overlap 0.
        Check(HandoffPropReady(true, true, 1, 0, 0) && !HandoffPropReady(false, true, 0, 0, 0), "handoff 0: hidden once Ready");
        // Overlap 1, a posed prop: armed and published in tick 1, the stand-in hidden in tick 2.
        Check(!HandoffPropReady(true, true, 1, 1, 1), "handoff 1: the stand-in stays in the tick of the first published pose");
        Check(HandoffPropReady(true, true, 2, 2, 1), "handoff 1: hidden the tick after");
        // A prop with a pose track not armed yet waits a few ticks more.
        Check(!HandoffPropReady(true, true, 3, 0, 1) && HandoffPropReady(true, true, 5, 0, 1),
            "handoff 1: a pose track not armed yet gets the grace ticks");
        // A prop without a pose track counts from Ready.
        Check(!HandoffPropReady(true, false, 1, 0, 1) && HandoffPropReady(true, false, 2, 0, 1),
            "handoff 1: a prop without a pose track, from Ready");
        Check(!HandoffPropReady(false, true, 9, 9, 1), "handoff: never before the prop is Ready");
        // SceneryShown with the handoff: shown in the takeover tick, gone after.
        Check(objcheck::SceneryShown(0, objcheck::kNeverGone, true, 1000, HandoffPropReady(true, true, 1, 1, 1), 1016),
            "handoff: SceneryShown keeps the stand-in in the overlap tick");
        Check(!objcheck::SceneryShown(0, objcheck::kNeverGone, true, 1000, HandoffPropReady(true, true, 2, 2, 1), 1033),
            "handoff: SceneryShown hides it the tick after");
    }
}

int RunObjectLookTests()
{
    g_lookFailures = 0;
    TestTintBits();
    TestNote();
    TestLookAt();
    TestTxds();
    TestDetector();
    TestHandoff();
    if (!g_lookFailures)
        std::printf("Object look tests passed.\n");
    return g_lookFailures;
}
