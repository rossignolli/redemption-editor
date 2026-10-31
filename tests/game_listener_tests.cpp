// The recorded live volume listener and when the replay puts it back
// (game_listener_core.h).
#include "game_listener_core.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace
{
    using namespace replay::gamelistener;

    int g_listenerFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_listenerFailures;
    }

    bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

    // Byte n of a signature's pattern is fixed and equals `value`.
    bool PatternHas(const replay::sig::Signature& s, std::size_t n, std::uint32_t value)
    {
        replay::sig::Pattern p;
        return replay::sig::Parse(s.pattern, p) && n < p.size && !p.wild[n] && p.bytes[n] == value;
    }

    Sample S(std::uint32_t t, float x, float y = 0.0f, float z = 0.0f)
    {
        Sample s;
        s.timeMs = t;
        s.pos[0] = x;
        s.pos[1] = y;
        s.pos[2] = z;
        s.points = 1;
        return s;
    }

    void TestSites()
    {
        Check(kEnvironment.parent == &kEnvLea && kEnvironment.via == replay::sig::Via::Rel32 && kEnvironment.at == 3 &&
                  kEnvironment.end == 7 && !*kEnvironment.pattern,
            "env lea target");
        Check(ListenerOffset(0) == 0x20 && ListenerOffset(1) == 0x330, "listener descriptions");
        replay::sig::Pattern lea;
        Check(replay::sig::Parse(kEnvLea.pattern, lea), "env lea parses");
        const std::uint8_t good[16] = {0x48, 0x8D, 0x0D, 0xA6, 0x8D, 0xA0, 0x02};
        const std::uint8_t moved[16] = {0x48, 0x8D, 0x0D, 0x00, 0x11, 0x22, 0x03};
        const std::uint8_t bad[16] = {0x48, 0x8D, 0x05, 0xA6, 0x8D, 0xA0, 0x02};
        Check(replay::sig::MatchesAt(lea, good, sizeof(good)) && replay::sig::MatchesAt(lea, moved, sizeof(moved)) &&
                  !replay::sig::MatchesAt(lea, bad, sizeof(bad)) && !replay::sig::MatchesAt(lea, nullptr, 16),
            "site match");
        for (const replay::sig::Signature* site : kSites)
        {
            replay::sig::Pattern p;
            Check(!*site->pattern || (replay::sig::Parse(site->pattern, p) && p.size <= 64), "site pattern");
        }
    }

    void TestTrack()
    {
        Track t;
        Check(SampleDue(t, 0), "empty: due");
        Append(t, S(0, 1.0f));
        Check(!SampleDue(t, 20) && SampleDue(t, 33), "period");
        Append(t, S(33, 2.0f));
        Append(t, S(66, 3.0f));
        Append(t, S(50, 9.0f)); // the clock went back: the newer tail goes
        Check(t.samples.size() == 3 && t.samples.back().timeMs == 50, "rewind drops the newer tail");

        Track u;
        Append(u, S(1000, 0.0f));
        Append(u, S(1100, 1.0f));
        Append(u, S(1200, 50.0f)); // a cut
        Append(u, S(1300, 51.0f));
        float out[3];
        Check(At(u, 1050, out) && Near(out[0], 0.5f), "interpolated inside a short step");
        Check(At(u, 1150, out) && Near(out[0], 1.0f), "a jump holds the earlier sample");
        Check(At(u, 1250, out) && Near(out[0], 50.5f), "after the cut");
        Check(At(u, 900, out) && Near(out[0], 0.0f), "before the first, near");
        Check(!At(u, 400, out), "before the first, stale");
        Check(At(u, 1700, out) && Near(out[0], 51.0f), "after the last, near");
        Check(!At(u, 1900, out), "after the last, stale");
        Track gap;
        Append(gap, S(0, 0.0f));
        Append(gap, S(1000, 1.0f));
        Check(At(gap, 300, out) && Near(out[0], 0.0f), "long gap holds");
        Check(!At(gap, 700, out), "long gap stale in the middle");
        Check(!At(Track{}, 0, out), "empty track");

        Track cut;
        u.CutInto(cut, 1100, 1250);
        Check(cut.samples.size() == 2 && cut.samples[0].timeMs == 0 && cut.samples[1].timeMs == 100, "cut window");
        u.DropBefore(1200);
        Check(u.samples.size() == 2 && u.samples[0].timeMs == 1200, "drop before");
    }
}

namespace
{
    // The microphone put back.
    void TestMicSites()
    {
        Check(kMicTable.parent == &kMicTableLea && kMicTable.via == replay::sig::Via::Rel32 && !*kMicTable.pattern,
            "mic table lea");
        Check(kMicMap.parent == &kMicMapCall && kMicMap.via == replay::sig::Via::Rel32 && kMicMap.at == 1 &&
                  kMicMap.end == 5,
            "mic map call");
        Check(SlotOffset(0) + 0x40 == kVolumePointPos && SlotOffset(1) == 0x220, "slots");
        Check(std::string(kMicCrossfadeRead.name) == "mic_crossfade_read" && PatternHas(kMicCrossfadeRead, 2, 0x78),
            "crossfade site");
        // The camera map's answer goes to the reference lookup, not the hash
        // lookup.
        Check(std::string(kMicMapRefLookup.name) == "mic_map_ref_lookup" &&
                  kMetaRefLookup.parent == &kMicMapRefLookup && kMetaRefLookup.rva == 0x252B5ACu &&
                  kMetaRefLookup.rva != kMetadataLookup.rva,
            "the map answer is a reference");
        Check(PatternHas(kMetaChunkStrideSite, 3, kMetaChunkStride) &&
                  PatternHas(kMetaChunkSizeSite, 4, kMetaChunkSize) &&
                  PatternHas(kMetaChunkDataSite, 4, kMetaChunkData) &&
                  PatternHas(kMetaEntryCountSite, 3, kMetaEntryCount) &&
                  PatternHas(kMetaChunkLoadedSite, 3, kMetaLoaded) && PatternHas(kMetaEntriesSite, 3, kMetaEntries),
            "metadata table layout sites");
        Check(PatternHas(kMicOverrideRead, 2, kMicOverrideHash & 0xFF) &&
                  PatternHas(kMicOverrideRead, 3, kMicOverrideHash >> 8) &&
                  PatternHas(kVolOverrideRead, 2, kFlags & 0xFF) &&
                  PatternHas(kVolMatrixRead, 3, kVolumeOverrideMatrix & 0xFF),
            "written offsets are in the sites");
        const MetaEntry entries[] = {{0x10, 0xAAAAu}, {0x40, 0x2A7C18A2u}, {0x80, 0xBBBBu}};
        Check(HashOfOffset(entries, 3, 0x40) == 0x2A7C18A2u && HashOfOffset(entries, 3, 0x44) == 0 &&
                  HashOfOffset(entries, 0, 0x10) == 0,
            "hash of an offset");
    }

    void TestMicAt()
    {
        Track t;
        Sample a = S(1000, 0.0f);
        a.points = 2; // v1 sample of 2 points (the 1152)
        Sample b = S(1500, 0.0f);
        b.points = 1;
        Sample c = S(2000, 0.0f);
        c.points = 1;
        c.micSource = MicSource::CameraMap;
        c.micHash = 0x1234u;
        Append(t, a);
        Append(t, b);
        Append(t, c);
        MicPlan p = MicAt(t, 1200, 0xABCDu);
        Check(p.who == MicWho::Derived && p.hash == 0xABCDu && p.pointsRec == 2, "v1 two points: derived fallback");
        p = MicAt(t, 1200, 0);
        Check(p.who == MicWho::None && p.hash == 0 && p.haveSample, "v1 two points without fallback: none");
        p = MicAt(t, 1700, 0xABCDu);
        Check(p.who == MicWho::None && p.pointsRec == 1, "v1 one point: left to the camera");
        p = MicAt(t, 2100, 0xABCDu);
        Check(p.who == MicWho::Recorded && p.hash == 0x1234u, "v2: the recorded hash wins");
        p = MicAt(t, 2600, 0xABCDu);
        Check(!p.haveSample && p.who == MicWho::None, "stale");
        Check(SampleAt(t, 900) == &t.samples[0] && SampleAt(t, 400) == nullptr, "before the first");
        Check(!HasV2(a) && HasV2(c) && t.AnyV2(), "v2 detection");
    }

    void TestFollows()
    {
        Check(FollowsRecordedCamera(true, false, -1.0f, 1.0f), "replay source");
        Check(FollowsRecordedCamera(false, true, 0.6f, 1.0f), "marker near the recorded camera");
        Check(!FollowsRecordedCamera(false, true, 1.5f, 1.0f), "marker far");
        Check(!FollowsRecordedCamera(false, true, -1.0f, 1.0f), "marker unknown");
        Check(!FollowsRecordedCamera(false, false, 0.0f, 1.0f), "free camera");
    }

    void TestDecideMic()
    {
        const char* why = "";
        MicInputs in;
        in.enabled = true;
        in.proven = true;
        in.follows = true;
        in.plan = 0x10u;
        Check(DecideMic(in, why) == MicAction::Write && std::string(why) == "apply", "write over 0");
        in.ours = 0x10u;
        in.current = 0x10u;
        Check(DecideMic(in, why) == MicAction::Keep && std::string(why) == "applied", "applied");
        in.plan = 0x20u;
        Check(DecideMic(in, why) == MicAction::Write, "a new hash over ours");
        in.plan = 0;
        Check(DecideMic(in, why) == MicAction::Restore && std::string(why) == "no_hash", "no hash: give back");
        in.plan = 0x10u;
        in.follows = false;
        Check(DecideMic(in, why) == MicAction::Restore && std::string(why) == "not_recorded_camera", "camera left: give back");
        in.follows = true;
        in.current = 0x99u; // a scene wrote over ours
        Check(DecideMic(in, why) == MicAction::Forget && std::string(why) == "taken_by_engine", "taken by the engine");
        in.ours = 0;
        Check(DecideMic(in, why) == MicAction::Keep && std::string(why) == "engine_override", "never over the engine's");
        in.current = 0;
        in.enabled = false;
        Check(DecideMic(in, why) == MicAction::Keep && std::string(why) == "off", "off");
        in.ours = 0x10u;
        in.current = 0x10u;
        Check(DecideMic(in, why) == MicAction::Restore, "off with ours: give back");
        Check(std::string(MicActionName(MicAction::Forget)) == "forget" && std::string(MicSourceName(MicSource::Default)) == "default" &&
                  std::string(MicWhoName(MicWho::Derived)) == "derived",
            "names");
    }
}

int RunGameListenerTests()
{
    TestSites();
    TestTrack();
    TestMicSites();
    TestMicAt();
    TestFollows();
    TestDecideMic();
    return g_listenerFailures;
}
