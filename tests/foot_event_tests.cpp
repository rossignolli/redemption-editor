// The secondary steps of the feet (the second queue of the foot audio) as
// events (foot_event_core.h): the engine sites, the queue entry, the track
// (add, ring, cut, validation, range), the playback rules, the evidence lines;
// and the sites of the movement component's speed (MotionSpeedReplay).
#include "foot_event_core.h"
#include "ped_audio_state.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay::footevent;

    int g_footEventFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_footEventFailures;
    }

    bool Has(const std::string& line, const char* part) { return line.find(part) != std::string::npos; }

    Event MakeEvent(std::uint32_t owner, std::uint32_t timeMs, std::int8_t type, std::uint8_t foot)
    {
        Event e;
        e.owner = owner;
        e.timeMs = timeMs;
        e.type = type;
        e.foot = foot;
        e.origin = static_cast<std::uint8_t>(Origin::Drain);
        e.materialHash = 0x11223344u;
        e.materialRef = 0x002F49ACu;
        return e;
    }

    void TestSites()
    {
        Check(sizeof(kEnqueueSignature) == 37 && kEnqueueSignature[0] == 0x48 && kEnqueueSignature[17] == 0xA7 &&
                  kEnqueueSignature[18] == 0x05,
            "the entry's prologue (37 bytes) tests +0x5A7");
        Check(CallTarget(kGeneratorCallRva, kGeneratorCallSignature) == kEnqueueRva,
            "0x8ED457 calls 0x317A1D0 (the generator)");
        Check(CallTarget(kDrainCallRva, kDrainCallSignature) == kEnqueueRva, "0x3193D96 calls 0x317A1D0 (the drain)");
        Check(kGeneratorReturnRva == kGeneratorCallRva + 5 && kDrainReturnRva == kDrainCallRva + 5, "the returns");
        Check(kEnqueueStoreRva > kEnqueueRva && kEnqueueStoreRva < kEnqueueRva + 0x16E, "the store inside the entry");
        // The store writes +0xD4 (count) and +0xB4 (entries, 8 bytes each).
        Check(kEnqueueStoreSignature[3] == (kQueueCountOffset & 0xFF) && kEnqueueStoreSignature[16] == (kQueueOffset & 0xFF),
            "the store's offsets are the queue's");
        Check(kQueueOffset + kQueueStride * kQueueMax == kQueueCountOffset, "four entries end at the count");
        Check(OriginOf(kGeneratorReturnRva) == Origin::Generator && OriginOf(kDrainReturnRva) == Origin::Drain &&
                  OriginOf(0x1234) == Origin::Other,
            "origin by return address");
        Check(std::string(OriginName(Origin::Generator)) == "gen" && std::string(OriginName(Origin::Drain)) == "drain",
            "origin names");

        // MotionSpeedReplay: the writer and the sound call the source.
        using namespace replay::pedaudiostate;
        Check(sizeof(kMotionSpeedSourceSignature) == 29 && sizeof(kMotionSpeedWriterSignature) == 31,
            "the speed source's and the writer's signatures");
        Check(CallTarget(kMotionSpeedWriterRva, kMotionSpeedWriterSignature) == kMotionSpeedSourceRva,
            "0x8EC5BA calls 0x8AABCC");
        Check(CallTarget(kMotionSpeedSoundRva, kMotionSpeedSoundSignature) == kMotionSpeedSourceRva,
            "0x31A16D3 calls 0x8AABCC");
        Check(CallTarget(kMotionSpeedWriterRva + 18, kMotionSpeedWriterSignature + 18) == 0x2521410,
            "the writer smooths through 0x2521410");
        Check(kMotionSpeedWriterSignature[27] == (kMotionSpeedOffset & 0xFF) &&
                  kMotionSpeedWriterSignature[28] == (kMotionSpeedOffset >> 8) &&
                  kMotionGaitReadSignature[4] == (kMotionSpeedOffset & 0xFF) && kMotionGaitReadSignature[5] == (kMotionSpeedOffset >> 8),
            "the writer and the gait read use +0x5DC");
        Check(kSpeedOffset == kMotionSpeedOffset, "the MOTION_STATE speed is the one the detour feeds");
        Check(kMaxMotionTargets >= 128, "room for every puppet");
    }

    void TestQueueEntry()
    {
        const std::uint8_t raw[kQueueStride] = {0x08, 0x00, 0x00, 0x00, 0x02, 0xAA, 0xBB, 0xCC};
        std::int32_t type = -1;
        std::uint8_t foot = 0xFF;
        DecodeQueueEntry(raw, type, foot);
        Check(type == 8 && foot == 2, "entry {8, 2} (the garbage bytes ignored)");
        const std::uint8_t minus[kQueueStride] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0, 0, 0};
        DecodeQueueEntry(minus, type, foot);
        Check(type == -1 && !ValidType(type), "type -1 is no event");
        Check(ValidType(0) && ValidType(11) && !ValidType(12), "types 0..11");
    }

    void TestTrack()
    {
        Track t;
        Check(t.Empty(), "empty");
        Check(t.Add(MakeEvent(0x10001u, 100, 8, 0)) && t.Add(MakeEvent(0x10001u, 150, 2, 1)), "add in order");
        Check(t.Add(MakeEvent(0x20003u, 120, 9, 3)), "a late time is clamped");
        Check(t.events[2].timeMs == 150, "clamped to the last");
        Check(!t.Add(MakeEvent(0x10001u, 200, -1, 0)) && !t.Add(MakeEvent(0x10001u, 200, 12, 0)), "bad type refused");
        Check(!t.Add(MakeEvent(0x10001u, 200, 3, kMaxFoot)), "foot 12 refused");
        Event bad = MakeEvent(0x10001u, 200, 3, 0);
        bad.flags = kFlagImpact;
        bad.impact = NAN;
        Check(!t.Add(bad), "a rise that is not a number refused");
        bad.flags = 0x80;
        bad.impact = 0.0f;
        Check(!t.Add(bad), "unknown flags refused");
        Event withRise = MakeEvent(0x10001u, 400, 6, 2);
        withRise.flags = kFlagImpact;
        withRise.impact = 0.37f;
        Check(t.Add(withRise), "a rise kept");
        Check(t.Valid() && t.events.size() == 4 && t.Owners() == 2, "valid, four events, two owners");

        std::vector<std::size_t> due;
        t.Range(100, 150, due);
        Check(due.size() == 2 && due[0] == 1 && due[1] == 2, "range (100, 150]");
        t.Range(150, 150, due);
        Check(due.empty(), "empty range");
        t.Range(0, 1000, due);
        Check(due.size() == 4, "the whole range");

        Track cut;
        t.CutInto(cut, 120, 400, [](std::uint32_t owner) { return owner == 0x10001u; });
        Check(cut.events.size() == 2 && cut.events[0].timeMs == 30 && cut.events[1].timeMs == 280 &&
                  cut.events[1].flags == kFlagImpact && cut.events[1].impact == 0.37f,
            "cut: the kept owner's events in the window, shifted");
        Track none;
        t.CutInto(none, 500, 900, [](std::uint32_t) { return true; });
        Check(none.Empty(), "cut: nothing before the base moves in (one-shots)");

        Track ring = t;
        Check(ring.DropBefore(150) == 1 && ring.events.front().timeMs == 150 && ring.events.size() == 3, "ring drop");
        Check(ring.DropBefore(10000) == 3 && ring.Empty(), "ring drops all");

        Track broken = t;
        broken.events[1].timeMs = 50;
        Check(!broken.Valid(), "a time going back is not valid");
        broken = t;
        broken.events[0].origin = 9;
        Check(!broken.Valid(), "an unknown origin is not valid");
    }

    void TestRules()
    {
        Check(RefuseOwn(true, true) && !RefuseOwn(false, true) && !RefuseOwn(true, false),
            "the puppet's own events are refused only with the section and the replay on (1152 keeps them)");
        const Event e = MakeEvent(1, 10, 8, 0);
        Check(Dispatch(true, true, e), "normal play dispatches");
        Check(!Dispatch(true, false, e), "a pause, scrub or seek does not (IsJustPlaying)");
        Check(!Dispatch(false, true, e), "FootEventReplay=0 does not");
        Check(!Dispatch(true, true, MakeEvent(1, 10, 8, 4)), "a foot the consumer never plays is not queued");
        Check(std::string(PlayStatusName(PlayStatus::Queued)) == "queued" &&
                  std::string(PlayStatusName(PlayStatus::QueueFull)) == "queue_full" &&
                  std::string(PlayStatusName(PlayStatus::NotQueued)) == "not_queued",
            "status names");
    }

    void TestWindows()
    {
        WindowCounts c;
        Check(!c.Any() && TypesText(c) == "none", "empty window");
        c.AddCall(Origin::Generator);
        c.AddCall(Origin::Drain);
        c.AddCall(Origin::Other);
        c.enq = 2;
        c.AddType(8);
        c.AddType(8);
        c.AddType(2);
        c.AddType(-1);
        Check(c.Any() && c.calls == 3 && c.gen == 1 && c.drain == 1 && c.other == 1, "calls by origin");
        Check(TypesText(c) == "2:1|8:2|bad:1", "types text");
        const std::string line = WindowLine("rec", 5000, "0:1", c);
        Check(Has(line, "FOOT_EVENT_WINDOW,phase=rec,clip_ms=5000,owner=0:1,calls=3,gen=1,drain=1,other=1,enq=2,") &&
                  Has(line, ",types=2:1|8:2|bad:1"),
            "window line");

        OwnerWindows windows;
        std::vector<std::string> emitted;
        const auto emit = [&](std::uint32_t windowMs, std::uint32_t owner, const WindowCounts& counts) {
            emitted.push_back(std::to_string(windowMs) + ":" + std::to_string(owner) + ":" + std::to_string(counts.replayed));
        };
        ++windows.At(7, 100, emit).replayed;
        ++windows.At(7, 4900, emit).replayed;
        ++windows.At(9, 4900, emit).replayed;
        Check(emitted.empty(), "same window: nothing emitted");
        windows.At(7, 5000, emit);
        Check(emitted.size() == 1 && emitted[0] == "0:7:2", "a new window emits the finished one");
        windows.At(7, 10000, emit); // the 5000 window saw nothing: not emitted
        Check(emitted.size() == 1, "an empty window is not emitted");
        windows.Flush(emit);
        Check(emitted.size() == 2 && emitted[1] == "0:9:1" && windows.owners.empty(), "flush emits what is left");

        Totals t;
        t.recorded = 527;
        t.played = 500;
        const std::string total = TotalLine("play", true, "", true, true, true, true, 527, 6, t);
        Check(Has(total, "FOOT_EVENT_TOTAL,phase=play,installed=1,error=none,record=1,replay=1,section=FSEV,refuse_own=1,"
                         "clip_events=527,clip_owners=6,") &&
                  Has(total, ",played=500,"),
            "total line");
        const std::string none = TotalLine("play", false, "foot_event_site_mismatch", true, true, false, false, 0, 0, Totals{});
        Check(Has(none, "installed=0,error=foot_event_site_mismatch") && Has(none, "section=none,refuse_own=0"),
            "total line without the section");
    }

    void TestPaths()
    {
        std::uint8_t field = 0;
        std::uint32_t container = 0;
        Check(IndirectField(0x800002002f49acull, field, container) && field == 0x02 && container == 0x002f49acu,
            "the live secondary step of clip 1152: field 02");
        Check(IndirectField(0x800004002f49acull, field, container) && field == 0x04, "the puppet's: field 04");
        Check(!IndirectField(0x00000000deadbeefull, field, container), "a plain ref has no field");

        PathCounts c;
        Check(!c.Any(), "no requests");
        c.Add(kMainStepCaller, 0x800006002f49acull);
        c.Add(kSecondaryStepCaller, 0x800002002f49acull);
        c.Add(kSecondaryStepCaller, 0x800002002f49acull);
        c.Add(kSecondaryStepCaller, 0x800004002f49acull);
        c.Add(kWetLayerCaller, 0x80000100299349aull);
        c.Add(0x31942A2u, 0x1234u);
        Check(c.requests == 6 && c.main == 1 && c.secondary == 3 && c.wet == 1 && c.other == 1, "paths");
        const std::string line = PathLine("rec", 30000, "0:1", c);
        Check(Has(line, "HORSE_STEP_PATHS,phase=rec,clip_ms=30000,owner=0:1,requests=6,main=1,secondary=3,wet=1,other=1,"
                        "sec_fields=02:2|04:1"),
            "path line");
        Check(Has(PathLine("play", 0, "0:1", PathCounts{}), "sec_fields=none"), "path line without secondaries");
    }

    void TestMotionLine()
    {
        MotionState s;
        Check(Has(MotionLine("play", 1000, "0:1", "horse", s, "engine"), "MOTION_STATE,phase=play,clip_ms=1000,owner=0:1,kind=horse,read=0"),
            "unread state");
        s.read = true;
        s.speed = 7.25f;
        s.gait = 5;
        s.gaitLoaded = 5;
        s.tableKnown = true;
        s.t[0] = 0.5f;
        s.t[1] = 2.0f;
        s.t[2] = 4.0f;
        s.t[3] = 8.0f;
        s.mountLink = true;
        s.thrKnown = true;
        s.thr = 0.32f;
        s.feetKnown = true;
        s.height[0] = 0.1f;
        s.rise[0] = 0.6f;
        s.last[0] = 2;
        s.contactsA = 12;
        const std::string line = MotionLine("rec", 2000, "0:1", "horse", s, "replay");
        Check(Has(line, "MOTION_STATE,phase=rec,clip_ms=2000,owner=0:1,kind=horse,speed=7.25,gait=5,gait_loaded=5,"
                        "tbl=0.50/2.00/4.00/8.00,mount_link=1,d0=none,thr=0.320,h=0.10/0.00/0.00/0.00,rise=0.60/") &&
                  Has(line, "last=2/0/0/0,contacts=12/0,b63e=0,src=replay"),
            "state line");
        s.feetKnown = false;
        Check(Has(MotionLine("rec", 0, "0:1", "horse", s, "engine"), ",feet=none,"), "feet unknown");
        Check(kFootStateOffset + 4 == kFootRiseOffset && kFootRiseOffset + 4 == kFootHeightOffset && kFootStride == 0x90,
            "the per-foot block of the component");
    }
}

namespace
{
    void TestMovementDetail()
    {
        Check(kMovementLimbComponentOffset + 0x24 == kMovementLimbGateOffset, "the gate is the limb component's +0x24");
        MovementWindow w;
        Check(!w.Any(), "empty movement window");
        w.Add(1.0f, 2.06f, 0, true, 0.058f);
        w.Add(3.0f, 2.06f, 1, true, 0.058f);
        w.Add(NAN, 5.0f, 1, false, 0.0f); // not a number: left out
        w.Add(2.0f, -1.0f, -1, false, 0.0f);
        Check(w.n == 3 && w.gateKnown == 2 && w.gateSet == 1 && w.f504Known == 2, "counts");
        const std::string line = MovementLine("play", 30000, "0:1", w);
        Check(Has(line, "MOVEMENT_TERM_DETAIL,phase=play,clip_ms=30000,owner=0:1,n=3,value_mean=2.000,result_mean=1.040,"
                        "result_min=-1.000,result_max=2.060,gate774=1/2,f504_mean=0.058"),
            "movement line");
        Check(Has(MovementLine("rec", 0, "0:1", MovementWindow{}), "n=0") &&
                  Has(MovementLine("rec", 0, "0:1", MovementWindow{}), "f504_mean=none"),
            "empty movement line");
    }
}

int RunFootEventTests()
{
    TestMovementDetail();
    TestSites();
    TestQueueEntry();
    TestTrack();
    TestRules();
    TestWindows();
    TestPaths();
    TestMotionLine();
    if (g_footEventFailures == 0)
        std::printf("foot event tests passed\n");
    return g_footEventFailures;
}
