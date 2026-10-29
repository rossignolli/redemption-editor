// Speech slots:
//   the replay's management of the ambient speech slots (speech_slots.h):
//   the priority by distance and screen, the gate's decision (ask, wait,
//   take the weakest replay line's slot, drop a line nobody hears or one
//   with too little left), the waiting order, the idle release, the ambient
//   source of the request;
//   lines already playing when the recording or the saved window began
//   (speech_resume.h OffsetAt, AdoptAtBase, EndOf and ActiveAt with
//   startedBeforeMs);
//   the state 3 handler's failure codes and signatures (world_theatre.h).
#include "speech_slots.h"
#include "speech_resume.h"
#include "world_theatre.h"
#include "log_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_failuresSpeechSlots = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failuresSpeechSlots;
    }

    bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

    namespace slots = replay::speech::slots;
    namespace resume = replay::speech::resume;
    using replay::entity::ReplayId;
    using replay::entity::SpeechEvent;

    void TestPriority()
    {
        Check(Near(slots::Priority(0.0f, false, 60.0f), 1.0f), "priority: at the camera 1");
        Check(Near(slots::Priority(30.0f, false, 60.0f), 0.5f), "priority: half the audible distance 0.5");
        Check(Near(slots::Priority(30.0f, true, 60.0f), 1.0f), "priority: on screen doubles");
        Check(Near(slots::Priority(59.9f, false, 60.0f), slots::kFloor), "priority: the floor near the edge");
        Check(slots::Priority(60.0f, true, 60.0f) == 0.0f, "priority: 0 at and beyond the audible distance");
        Check(slots::Priority(10.0f, false, 60.0f) > slots::Priority(20.0f, false, 60.0f), "priority: nearer is stronger");
        Check(slots::Priority(25.0f, true, 60.0f) > slots::Priority(15.0f, false, 60.0f),
            "priority: on screen at 25 m beats off screen at 15 m");
        Check(Near(slots::Priority(-1.0f, false, 60.0f), 1.0f), "priority: unknown distance is not penalised");
        Check(Near(slots::Priority(100.0f, false, 0.0f), 1.0f), "priority: audible distance 0 turns the curve off");
        Check(slots::Audible(59.0f, 60.0f) && !slots::Audible(60.0f, 60.0f) && slots::Audible(500.0f, 0.0f),
            "audible: by the distance, always with 0");
    }

    void TestDecide()
    {
        std::vector<slots::Holder> none;
        slots::Verdict v = slots::Decide(-1, 1.0f, true, 1000, true, none);
        Check(v.decision == slots::Decision::Ask, "gate: census unreadable asks as before");
        v = slots::Decide(3, 0.5f, true, 1000, true, none);
        Check(v.decision == slots::Decision::Ask, "gate: a free slot asks");
        v = slots::Decide(3, 0.5f, true, 100, true, none);
        Check(v.decision == slots::Decision::DropLate, "gate: too little left is dropped");
        // A short line asked on time is not late.
        Check(slots::Decide(8, 1.9f, true, 93, true, none, 7).decision == slots::Decision::Ask,
            "decide: a 100 ms line asked 7 ms after its start is asked");
        Check(slots::Decide(8, 1.9f, true, 93, true, none, 600).decision == slots::Decision::DropLate,
            "decide: the same line asked 600 ms late is dropped");
        v = slots::Decide(3, 0.5f, true, 100, false, none);
        Check(v.decision == slots::Decision::Ask, "gate: an unknown remainder is not dropped for it");
        v = slots::Decide(1, 0.0f, false, 1000, true, none);
        Check(v.decision == slots::Decision::DropInaudible, "gate: inaudible with slots short is not asked");
        v = slots::Decide(slots::kReserveForAudible, 0.0f, false, 1000, true, none);
        Check(v.decision == slots::Decision::Ask, "gate: inaudible with enough slots free is asked");
        v = slots::Decide(0, 0.8f, true, 1000, true, none);
        Check(v.decision == slots::Decision::Wait, "gate: no slot and no replay holder waits");

        std::vector<slots::Holder> holders;
        holders.push_back({4, 0.9f});
        holders.push_back({7, 0.2f});
        holders.push_back({9, 0.6f});
        v = slots::Decide(0, 0.8f, true, 1000, true, holders);
        Check(v.decision == slots::Decision::Evict && v.victim == 7, "gate: the weakest replay line gives its slot");
        v = slots::Decide(0, 0.24f, true, 1000, true, holders);
        Check(v.decision == slots::Decision::Wait, "gate: not clearly stronger (margin) waits");
        v = slots::Decide(0, 0.26f, true, 1000, true, holders);
        Check(v.decision == slots::Decision::Evict && v.victim == 7, "gate: past the margin takes the slot");
        std::vector<slots::Holder> zero{{2, 0.0f}};
        v = slots::Decide(0, 0.0f, true, 1000, true, zero);
        Check(v.decision == slots::Decision::Wait, "gate: a 0 priority due line never takes a slot");
        v = slots::Decide(0, 0.1f, true, 1000, true, zero);
        Check(v.decision == slots::Decision::Evict && v.victim == 2, "gate: an inaudible holder gives way");
        Check(std::string(slots::DecisionName(slots::Decision::DropInaudible)) == "drop_inaudible" &&
                std::string(slots::DecisionName(slots::Decision::Evict)) == "evict",
            "gate: decision names for the log");
    }

    void TestOrderAndRelease()
    {
        std::vector<slots::Waiting> order{{0, 0.3f, 100}, {1, 0.9f, 300}, {2, 0.9f, 200}, {3, 0.1f, 50}};
        std::sort(order.begin(), order.end(), slots::Before);
        Check(order[0].id == 2 && order[1].id == 1 && order[2].id == 0 && order[3].id == 3,
            "queue: strongest first, then the oldest wait");
        Check(slots::ReleaseIdle(0, true, false), "release: a puppet's idle slot with slots short");
        Check(!slots::ReleaseIdle(0, true, true), "release: not while one of the replay's lines is on that puppet");
        Check(!slots::ReleaseIdle(0, false, false), "release: never a slot of the world");
        Check(!slots::ReleaseIdle(slots::kReserveForAudible, true, false), "release: not with enough slots free");
        Check(!slots::ReleaseIdle(-1, true, false), "release: not with the census unread");
    }

    void TestRequestSource()
    {
        Check(slots::RequestSource(true, false, false) == slots::kSourceAmbient, "source: a human line asks ambient");
        Check(slots::RequestSource(true, true, false) == 0, "source: an animal keeps its own pipeline");
        Check(slots::RequestSource(true, false, true) == 0, "source: a voice whose ambient load failed goes back");
        Check(slots::RequestSource(false, false, false) == 0, "source: SpeechAmbientSource=0 is the old request");
        // The wave slot retries alternate the sources.
        Check(slots::RetrySource(true, false, true, 1) == 0, "retry source: try 1 keeps the fallback's source");
        Check(slots::RetrySource(true, false, true, 2) == slots::kSourceAmbient, "retry source: try 2 asks ambient again");
        Check(slots::RetrySource(true, false, false, 1) == slots::kSourceAmbient, "retry source: try 1 of an ambient line");
        Check(slots::RetrySource(true, false, false, 2) == 0, "retry source: try 2 asks the metadata's source");
        Check(slots::RetrySource(false, false, false, 1) == 0, "retry source: SpeechAmbientSource=0, try 1 asks the metadata's source");
        Check(slots::RetrySource(false, false, false, 2) == slots::kSourceAmbient,
            "retry source: SpeechAmbientSource=0, try 2 asks the ambient slots");
        Check(slots::RetrySource(true, true, false, 2) == 0, "retry source: an animal never alternates");
        Check(slots::kParamsSource == 0xF8 && slots::kSourceBank == 3 && slots::kSourceAmbient == 4,
            "source: params +0xF8, bank 3, ambient 4 (offline 1491.50)");
        Check(slots::kAmbientRef == 0x10 && slots::kAmbientWave == 0x18 && slots::kAmbientOwner == 0x28 &&
                slots::kLineWaveRef == 0xD0,
            "slots: ambient slot +0x10 ref, +0x18 wave, +0x28 owner; line +0xD0");
    }

    SpeechEvent Line(std::uint16_t speaker, std::uint32_t startMs, std::uint32_t endMs, std::uint32_t before = 0)
    {
        SpeechEvent line;
        line.speaker = ReplayId{speaker, 1};
        line.timeMs = startMs;
        line.endMs = endMs;
        line.startedBeforeMs = before;
        return line;
    }

    void TestAdoptedOffsets()
    {
        // A line adopted at 0 that had played 2.4 s.
        const SpeechEvent adopted = Line(5, 0, 3000, 2400);
        Check(resume::OffsetAt(adopted, 0) == 2400, "offset: an adopted line goes on from where it was");
        Check(resume::OffsetAt(adopted, 500) == 2900, "offset: plus the playhead past its event");
        const SpeechEvent plain = Line(5, 1000, 3000);
        Check(resume::OffsetAt(plain, 1040) == 0, "offset: under 66 ms from the start");
        Check(resume::OffsetAt(plain, 1500) == 500, "offset: the playhead past the start");
        Check(resume::OffsetAt(plain, 900) == 0, "offset: before the start");
        const SpeechEvent tiny = Line(5, 0, 3000, 40);
        Check(resume::OffsetAt(tiny, 0) == 0, "offset: an adopted line under the floor plays from its start");

        // Unknown end: the cap counts what it had played.
        SpeechEvent unknown = Line(5, 0, 0, 10000);
        resume::LineEnd end = resume::EndOf(unknown, 0, 0);
        Check(end.endMs == 2000 && std::string(end.source) == "cap", "end: the cap of an adopted line is what was left");
        unknown.startedBeforeMs = 20000;
        end = resume::EndOf(unknown, 0, 0);
        Check(end.endMs == resume::kMinRemainingMs, "end: an adopted line past the cap keeps a blip");

        // The first resume of the playback asks it with the offset.
        std::vector<SpeechEvent> events{adopted, Line(6, 1500, 2500)};
        std::vector<resume::Active> active;
        resume::ActiveAt(events, 0, {}, {}, active);
        Check(active.size() == 1 && active[0].index == 0 && active[0].offsetMs == 2400 && active[0].remainingMs == 3000,
            "resume: the adopted line at 0 is said with its offset");
        resume::ActiveAt(events, 1000, {}, {}, active);
        Check(active.size() == 1 && active[0].offsetMs == 3400, "resume: later in it, offset grows");
    }

    void TestAdoptAtBase()
    {
        SpeechEvent out;
        SpeechEvent line = Line(5, 3000, 4600);
        line.coreMs = 2900;
        Check(resume::AdoptAtBase(line, 4000, 0, out) && out.timeMs == 0 && out.startedBeforeMs == 1000 &&
                out.endMs == 600 && out.coreMs == 0,
            "window: a line audible at the base is adopted at 0");
        Check(!resume::AdoptAtBase(Line(5, 3000, 4100), 4000, 0, out), "window: one ending within 250 ms is not");
        Check(!resume::AdoptAtBase(Line(5, 3000, 3900), 4000, 0, out), "window: one that ended is not");
        Check(!resume::AdoptAtBase(Line(5, 4000, 4600), 4000, 0, out), "window: one at the base is cut as usual");
        Check(!resume::AdoptAtBase(Line(5, 3000, 0), 4000, 4500, out), "window: an unknown end is not adopted");
        SpeechEvent stopped = Line(5, 3000, 0);
        stopped.stopMs = 5000;
        stopped.stopReason = 2;
        Check(resume::AdoptAtBase(stopped, 4000, 0, out) && out.stopMs == 1000 && out.stopReason == 2 && out.endMs == 0,
            "window: an early stop after the base keeps its reason, shifted");
        SpeechEvent again = Line(5, 3500, 6000, 700);
        Check(resume::AdoptAtBase(again, 4000, 0, out) && out.startedBeforeMs == 1200,
            "window: an adopted line cut again adds up");
    }

    void TestState3()
    {
        namespace theatre = replay::theatre;
        Check(theatre::SpeechCodeFailed(6) && theatre::SpeechCodeFailed(0xA) && theatre::SpeechCodeFailed(0xB) &&
                theatre::SpeechCodeFailed(0x45),
            "state 3: codes under 0x46 are failures (0x2F49A64)");
        Check(!theatre::SpeechCodeFailed(0x49) && !theatre::SpeechCodeFailed(0x4A) && !theatre::SpeechCodeFailed(0x4D),
            "state 3: 0x49 to 0x4D are success");
        Check(theatre::kSpeechState3Signature.rva == 0x2FC486C && theatre::kSpeechState3Signature.bytes[0] == 0x48 &&
                theatre::kSpeechState3Signature.bytes[15] == 0xA9,
            "state 3: the handler's prologue (lea rbp, [rsp-0x57])");
        Check(theatre::kAmbientReleaseSignature.rva == 0x2FB1E94 && theatre::kAmbientReleaseSignature.bytes[23] == 0x66,
            "release: 0x2FB1E94's prologue ends in the cmp of the slot count");
        Check(theatre::kAmbientArray == 0x3F0 && theatre::kAmbientCount == 0x3F8 && theatre::kAmbientStride == 0x38,
            "release: the manager's ambient array, count and stride");
        theatre::Config config;
        Check((config.hooks & theatre::kHookState3) != 0, "state 3: the handler's detour is on by default");
    }

    void TestLogLines()
    {
        using replay::log::EssentialLine;
        Check(EssentialLine("SPEECH_SLOT_GATE,ped=3:4,decision=wait") && EssentialLine("SPEECH_SLOT_EVICT,victim=1:2") &&
                EssentialLine("SPEECH_SLOT_RELEASE,owner=0x1"),
            "log: the slot gate's lines stay with Debug=0");
        Check(EssentialLine("POSE_V2_SPEECH_ADOPT,ped=3:4,started_before_ms=1200"), "log: an adopted line stays with Debug=0");
        Check(EssentialLine("POSE_V2_PLAYBACK_SPEECH_TOTAL,gate_waits=1") && EssentialLine("POSE_V2_SPEECH_RECORD_TOTAL,adopted=1"),
            "log: the totals with the new counters stay");
        Check(!EssentialLine("SPEECH_RESUME,ped=1:2"), "log: other evidence lines still need Debug=1");
    }
}

int RunSpeechSlotsTests()
{
    TestPriority();
    TestDecide();
    TestOrderAndRelease();
    TestRequestSource();
    TestAdoptedOffsets();
    TestAdoptAtBase();
    TestState3();
    TestLogLines();
    if (g_failuresSpeechSlots == 0)
        std::printf("speech slots and adopted lines tests passed\n");
    return g_failuresSpeechSlots;
}
