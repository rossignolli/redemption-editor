// The door track of vehicles and train cars (src/vehicle_doors.h) and the
// pose places of the train cars (src/train_consist.h).
#include "train_consist.h"
#include "vehicle_doors.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    using namespace replay;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

    void TestObserve()
    {
        vehdoors::DoorTrack track;
        track.id = 7;
        Check(vehdoors::Observe(track, 100, 0, 0.0f), "observe: the first read is stored (shut)");
        Check(!vehdoors::Observe(track, 150, 0, 0.0f), "observe: no change, nothing stored");
        Check(!vehdoors::Observe(track, 200, 0, 0.004f), "observe: below the step, nothing stored");
        // The door starts sliding at 250: the rest read at 200 goes in first.
        Check(vehdoors::Observe(track, 250, 0, 0.2f), "observe: the opening is stored");
        Check(track.samples.size() == 3 && track.samples[1].timeMs == 200 && Near(track.samples[1].ratio, 0.004f) &&
                track.samples[2].timeMs == 250 && Near(track.samples[2].ratio, 0.2f),
            "observe: the rest it left from, then the new ratio");
        Check(vehdoors::Observe(track, 300, 0, 0.6f) && track.samples.size() == 4, "observe: moving every read, one sample each");
        Check(vehdoors::Observe(track, 350, 0, 1.3f) && Near(track.samples.back().ratio, 1.0f), "observe: clamped to fully open");
        Check(!vehdoors::Observe(track, 400, 0, 0.995f), "observe: a wobble below the step at the end is not stored");
        Check(vehdoors::Observe(track, 100, 1, 0.5f), "observe: another door has its own first read");
        Check(vehdoors::Observe(track, 400, vehdoors::kLatchChannel, 1.0f), "observe: the latch channel");
        Check(!vehdoors::Observe(track, 400, 16, 1.0f), "observe: no channel past 15");
        Check(vehdoors::Channels(track) == ((1u << 0) | (1u << 1) | (1u << vehdoors::kLatchChannel)), "observe: the channels");
        Check(vehdoors::EverOpen(track), "observe: a door opened");
        vehdoors::DoorTrack shut;
        vehdoors::Observe(shut, 0, 2, 0.0f);
        vehdoors::Observe(shut, 0, vehdoors::kLatchChannel, 1.0f);
        Check(!vehdoors::EverOpen(shut), "observe: the latch is not a door opening");
    }

    void TestRatioAt()
    {
        vehdoors::DoorTrack track;
        track.samples = {{100, 0, 0.0f}, {200, 0, 0.0f}, {300, 0, 1.0f}, {300, 3, 0.5f}, {500, 0, 0.0f},
            {600, vehdoors::kLatchChannel, 0.0f}, {700, vehdoors::kLatchChannel, 1.0f}};
        float value = -1.0f;
        Check(vehdoors::RatioAt(track, 0, 50, value) && Near(value, 0.0f), "ratio: before the first sample, the first");
        Check(vehdoors::RatioAt(track, 0, 250, value) && Near(value, 0.5f), "ratio: interpolated while it slides");
        Check(vehdoors::RatioAt(track, 0, 400, value) && Near(value, 0.5f), "ratio: interpolated while it closes");
        Check(vehdoors::RatioAt(track, 0, 900, value) && Near(value, 0.0f), "ratio: after the last, the last");
        Check(vehdoors::RatioAt(track, 3, 0, value) && Near(value, 0.5f), "ratio: another door, its own samples");
        Check(vehdoors::RatioAt(track, vehdoors::kLatchChannel, 650, value) && Near(value, 0.0f), "ratio: the latch is held, not blended");
        Check(!vehdoors::RatioAt(track, 5, 300, value), "ratio: a door with no sample");
    }

    void TestDropBefore()
    {
        vehdoors::DoorTrack track;
        track.samples = {{100, 0, 0.0f}, {200, 1, 0.3f}, {300, 0, 1.0f}, {400, 1, 0.0f}, {500, 0, 0.5f}};
        const std::size_t dropped = vehdoors::DropBefore(track, 350);
        Check(dropped == 1, "drop: the older samples of each door go");
        Check(track.samples.size() == 4 && track.samples[0].timeMs == 200 && track.samples[1].timeMs == 300 &&
                track.samples[2].timeMs == 400 && track.samples[3].timeMs == 500,
            "drop: the last of each door at or before the cut stays");
        Check(vehdoors::DropBefore(track, 350) == 0, "drop: nothing more the second time");
    }

    void TestPlayRules()
    {
        Check(!vehdoors::NeedsDrive(0.5f, 0.502f, true), "drive: a door where the record has it is left alone");
        Check(vehdoors::NeedsDrive(0.5f, 0.0f, true), "drive: a shut door the record has open");
        Check(vehdoors::NeedsDrive(0.0f, 0.0f, false), "drive: an unreadable door is driven");
        Check(vehdoors::Judge(1.0f, 0.97f, true, 0) == vehdoors::Verdict::Match, "judge: within the tolerance");
        Check(vehdoors::Judge(1.0f, 0.2f, true, 200) == vehdoors::Verdict::Settling, "judge: a door still sliding");
        Check(vehdoors::Judge(1.0f, 0.2f, true, vehdoors::kSettleMs) == vehdoors::Verdict::Mismatch, "judge: shut after the settle");
        Check(vehdoors::Judge(1.0f, 0.2f, false, 5000) == vehdoors::Verdict::Unread, "judge: unread");
        Check(vehdoors::Quantize(0.12345f) == 0.123f && vehdoors::Quantize(-0.2f) == 0.0f, "quantize: a thousandth, clamped");
    }

    void TestPosePlaces()
    {
        Check(trains::ShouldPoseCar(78, 0, 20.0f), "pose: a near locomotive with bones");
        Check(!trains::ShouldPoseCar(1, 0, 20.0f), "pose: a rigid car (one bone) never");
        Check(!trains::ShouldPoseCar(78, trains::kPoseCars, 20.0f), "pose: no more than the places");
        Check(!trains::ShouldPoseCar(78, 0, trains::kPoseRadiusM + 1.0f), "pose: not past the radius");
        const std::vector<std::size_t> order = trains::PoseOrder({50.0f, 5.0f, 120.0f, 5.0f});
        Check(order.size() == 4 && order[0] == 1 && order[1] == 3 && order[2] == 0 && order[3] == 2,
            "pose: the nearest first, ties in walk order");
    }
}

int RunTrainReplayTests()
{
    g_failures = 0;
    TestObserve();
    TestRatioAt();
    TestDropBefore();
    TestPlayRules();
    TestPosePlaces();
    if (g_failures == 0)
        std::printf("train replay tests passed\n");
    return g_failures;
}
