// Trains (train_consist.h): the consist walk over the engine's car links, the
// consist decision, the clip's links, the head car's speed track, the window
// cut and the puppet judge (vehicle or object).
#include "train_consist.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

namespace
{
    int g_trainFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_trainFailures;
    }

    using replay::trains::CarLinks;
    using replay::trains::WalkStatus;

    // A fake engine: address -> links, as the reads at +0x11F0/+0x11F8 give.
    struct Links
    {
        std::map<std::uint64_t, CarLinks> cars;
        std::uint32_t reads = 0;

        void Chain(const std::vector<std::uint64_t>& frontFirst)
        {
            for (std::size_t n = 0; n < frontFirst.size(); ++n)
            {
                CarLinks& car = cars[frontFirst[n]];
                car.address = frontFirst[n];
                car.forward = n > 0 ? frontFirst[n - 1] : 0;
                car.backward = n + 1 < frontFirst.size() ? frontFirst[n + 1] : 0;
            }
        }
        bool operator()(std::uint64_t at, CarLinks& out)
        {
            ++reads;
            const auto found = cars.find(at);
            if (found == cars.end())
                return false;
            out = found->second;
            return true;
        }
    };

    std::vector<std::uint64_t> Addresses(const std::vector<CarLinks>& ordered)
    {
        std::vector<std::uint64_t> out;
        for (const CarLinks& car : ordered)
            out.push_back(car.address);
        return out;
    }

    void TestWalk()
    {
        const std::vector<std::uint64_t> train = {0x1000, 0x2000, 0x3000, 0x4000, 0x5000};
        std::vector<CarLinks> ordered;
        {
            Links links;
            links.Chain(train);
            Check(replay::trains::WalkConsist(0x3000, links, ordered) == WalkStatus::Ok, "walk: from a middle car");
            Check(Addresses(ordered) == train, "walk: the whole consist, front first, from a middle car");
            Check(replay::trains::WalkConsist(0x1000, links, ordered) == WalkStatus::Ok && Addresses(ordered) == train,
                "walk: from the head");
            Check(replay::trains::WalkConsist(0x5000, links, ordered) == WalkStatus::Ok && Addresses(ordered) == train,
                "walk: from the tail");
        }
        {
            Links links;
            links.Chain({0x1000});
            Check(replay::trains::WalkConsist(0x1000, links, ordered) == WalkStatus::Ok && ordered.size() == 1,
                "walk: a lone car (a handcar, a trolley)");
        }
        {
            Links links;
            Check(replay::trains::WalkConsist(0x1000, links, ordered) == WalkStatus::Unreadable && ordered.empty(),
                "walk: an unreadable start");
            Check(replay::trains::WalkConsist(0, links, ordered) == WalkStatus::Unreadable, "walk: no start");
        }
        {
            // A car behind that does not point back: the list is being
            // rebuilt; the good part is kept.
            Links links;
            links.Chain(train);
            links.cars[0x4000].forward = 0x9999;
            const WalkStatus status = replay::trains::WalkConsist(0x1000, links, ordered);
            Check(status == WalkStatus::Broken && Addresses(ordered) == std::vector<std::uint64_t>({0x1000, 0x2000, 0x3000}),
                "walk: a broken back link ends the walk there");
        }
        {
            // A front car that does not point back: the walk starts there.
            Links links;
            links.Chain(train);
            links.cars[0x2000].backward = 0x7777;
            const WalkStatus status = replay::trains::WalkConsist(0x4000, links, ordered);
            Check(status == WalkStatus::Broken && ordered.front().address == 0x3000 && ordered.back().address == 0x5000,
                "walk: a broken front link keeps the cars from the last good one");
        }
        {
            // A cycle through the forward links.
            Links links;
            links.Chain({0x1000, 0x2000, 0x3000});
            links.cars[0x1000].forward = 0x3000;
            links.cars[0x3000].backward = 0x1000;
            const WalkStatus status = replay::trains::WalkConsist(0x2000, links, ordered);
            Check(status == WalkStatus::Cycle || status == WalkStatus::Broken, "walk: a cycle stops");
            Check(links.reads < 16, "walk: a cycle does not loop");
        }
        {
            // A list longer than the cap.
            Links links;
            std::vector<std::uint64_t> longTrain;
            for (std::uint64_t n = 1; n <= replay::trains::kMaxConsistCars + 10; ++n)
                longTrain.push_back(n * 0x100);
            links.Chain(longTrain);
            const WalkStatus status = replay::trains::WalkConsist(longTrain.back(), links, ordered);
            Check(status == WalkStatus::TooLong && ordered.size() <= replay::trains::kMaxConsistCars, "walk: capped");
        }
        {
            // A car that becomes unreadable behind.
            Links links;
            links.Chain(train);
            links.cars.erase(0x5000);
            const WalkStatus status = replay::trains::WalkConsist(0x2000, links, ordered);
            Check(status == WalkStatus::Unreadable && ordered.size() == 4, "walk: an unreadable car behind keeps the rest");
        }
        Check(replay::trains::IsTrainVehicleType(17) && replay::trains::IsTrainVehicleType(19) &&
                !replay::trains::IsTrainVehicleType(16) && !replay::trains::IsTrainVehicleType(20) &&
                !replay::trains::IsTrainVehicleType(-1),
            "vehicle types 17..19 are trains");
    }

    void TestDecide()
    {
        using replay::trains::ConsistAction;
        using replay::trains::DecideConsist;
        Check(DecideConsist(false, 120.0f, 150.0f, 175.0f, true) == ConsistAction::Enter, "decide: a car in range enters it");
        Check(DecideConsist(false, 160.0f, 150.0f, 175.0f, true) == ConsistAction::Ignore, "decide: past the enter radius");
        Check(DecideConsist(true, 160.0f, 150.0f, 175.0f, true) == ConsistAction::Keep, "decide: kept up to the exit radius");
        Check(DecideConsist(true, 180.0f, 150.0f, 175.0f, true) == ConsistAction::Leave, "decide: every car past the exit radius");
        Check(DecideConsist(true, 10.0f, 150.0f, 175.0f, false) == ConsistAction::Leave, "decide: every car gone");
        Check(DecideConsist(false, 10.0f, 150.0f, 175.0f, false) == ConsistAction::Ignore, "decide: nothing alive, nothing enters");
    }

    void TestLinks()
    {
        std::vector<replay::trains::TrainCar> cars(4);
        cars[0].id = 30;
        cars[1].id = 10;
        cars[2].id = 20;
        cars[3].id = 99; // another consist
        cars[3].head = 99;
        cars[1].flags = replay::trains::kCarMission;
        replay::trains::LinkConsist({10, 20, 30}, cars);
        const auto find = [&](std::uint32_t id) -> const replay::trains::TrainCar& {
            for (const replay::trains::TrainCar& car : cars)
                if (car.id == id)
                    return car;
            return cars[3];
        };
        Check(find(10).head == 10 && find(20).head == 10 && find(30).head == 10, "links: the front car is the head");
        Check(find(10).index == 0 && find(20).index == 1 && find(30).index == 2 && find(20).cars == 3, "links: index from the front");
        Check(find(10).forward == 0 && find(10).backward == 20 && find(20).forward == 10 && find(20).backward == 30 &&
                find(30).backward == 0,
            "links: neighbours");
        Check((find(10).flags & replay::trains::kCarHead) && (find(30).flags & replay::trains::kCarTail) &&
                !(find(20).flags & (replay::trains::kCarHead | replay::trains::kCarTail)),
            "links: head and tail flags");
        Check((find(10).flags & replay::trains::kCarMission) != 0, "links: other flags kept");
        Check(find(99).head == 99 && find(99).index == 0 && find(99).forward == 0, "links: another consist untouched");
        // A car coupled in front later: linked again, the flags follow.
        replay::trains::LinkConsist({40, 10, 20, 30}, cars);
        Check(find(10).index == 1 && find(10).forward == 40 && !(find(10).flags & replay::trains::kCarHead) &&
                find(30).head == 40,
            "links: a car coupled on moves the others");
    }

    void TestSpeed()
    {
        replay::trains::SpeedTrack track;
        track.head = 7;
        Check(track.Append(0, 0.0f, true), "speed: the first sample is kept");
        Check(!track.Append(100, 5.0f, true), "speed: rate gated (250 ms)");
        Check(!track.Append(300, 0.01f, true), "speed: the same speed is not kept again");
        Check(track.Append(400, 4.0f, true), "speed: a change is kept");
        Check(track.Append(700, 4.0f, false), "speed: a direction change is kept");
        Check(!track.Append(1000, 4.02f, false), "speed: a tiny change is not");
        Check(track.Append(2800, 4.02f, false), "speed: kept at least every 2 s");
        float speed = 0.0f;
        bool forward = false;
        Check(track.At(200, speed, forward) && std::fabs(speed - 2.0f) < 1e-4f && forward, "speed: interpolated");
        Check(track.At(5000, speed, forward) && std::fabs(speed - 4.02f) < 1e-4f && !forward, "speed: clamped at the end");
        Check(track.At(0, speed, forward) && speed == 0.0f, "speed: at the start");
        Check(track.DropBefore(700) == 2 && track.samples.front().timeMs == 700, "speed: the buffer keeps the one in force");
        replay::trains::SpeedTrack empty;
        Check(!empty.At(0, speed, forward), "speed: an empty track");
    }

    void TestCut()
    {
        replay::trains::TrainData data;
        replay::trains::TrainCar a;
        a.id = 1;
        a.head = 1;
        a.backward = 2;
        replay::trains::TrainCar b;
        b.id = 2;
        b.head = 1;
        b.forward = 1;
        b.backward = 3;
        replay::trains::TrainCar c;
        c.id = 3;
        c.head = 1;
        c.forward = 2;
        data.cars = {a, b, c};
        replay::trains::SpeedTrack speed;
        speed.head = 1;
        speed.samples = {{0, 1.0f, 1}, {500, 2.0f, 1}, {1000, 3.0f, 1}};
        data.speeds.push_back(speed);
        replay::trains::TrainData out;
        // Car 1 left the window: 2 keeps no head and no car in front.
        data.CutInto(out, [](std::uint32_t id) { return id != 1; },
            [](const std::vector<replay::trains::SpeedSample>& samples) { return samples; });
        Check(out.cars.size() == 2 && out.Find(2) && out.Find(2)->forward == 0 && out.Find(2)->head == 0 &&
                out.Find(2)->backward == 3 && out.Find(3)->forward == 2,
            "cut: a neighbour out of the window reads 0");
        Check(out.speeds.empty(), "cut: the head's speed goes with the head");
        data.CutInto(out, [](std::uint32_t) { return true; },
            [](const std::vector<replay::trains::SpeedSample>& samples) {
                std::vector<replay::trains::SpeedSample> kept(samples.begin() + 1, samples.end());
                for (replay::trains::SpeedSample& sample : kept)
                    sample.timeMs -= 500;
                return kept;
            });
        Check(out.speeds.size() == 1 && out.speeds[0].samples.size() == 2 && out.speeds[0].samples[0].timeMs == 0,
            "cut: speed samples through the window's cut");
        Check(!data.Empty() && data.SpeedOf(1) && !data.SpeedOf(2), "data: lookups");
        data.Clear();
        Check(data.Empty(), "data: cleared");
    }

    void TestJudge()
    {
        replay::trains::PuppetJudge judge;
        Check(!judge.Refused() && !judge.Refused() && judge.Refused(), "judge: the third refusal turns it into an object");
        replay::trains::PuppetJudge drift;
        Check(!drift.Drift(0.1f) && !drift.Drift(2.0f) && !drift.Drift(2.0f) && drift.Drift(3.0f),
            "judge: three drifts in a row (pulled onto a track)");
        replay::trains::PuppetJudge flicker;
        Check(!flicker.Drift(2.0f) && !flicker.Drift(2.0f) && !flicker.Drift(0.0f) && !flicker.Drift(2.0f),
            "judge: a placement that holds resets the streak");
        Check(flicker.drifts == 3 && std::fabs(flicker.worstDriftM - 2.0f) < 1e-6f, "judge: counted and the worst kept");
    }
}

int RunTrainConsistTests()
{
    TestWalk();
    TestDecide();
    TestLinks();
    TestSpeed();
    TestCut();
    TestJudge();
    if (g_trainFailures == 0)
        std::puts("Train consist tests passed.");
    return g_trainFailures;
}
