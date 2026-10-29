#pragma once

// Trains in the replay.
//
// A consist is recorded whole as soon as one of its cars is a candidate:
// every car gets a rigid world track at the capture rate (no pose record, no
// skeleton place), and the clip keeps who is in front and behind, the index
// from the front, the cargo and lantern prop sets and the head car's speed
// and direction. Playback builds each car as an inert puppet placed on its
// recorded matrix every frame; the train system never drives it, and the
// world's own trains are cleared.
//
// Engine structure (build 1491.50): a car's
// vehicle type is the int at +0xBB0 (17..19 are trains); the car in front is
// the pointer at +0x11F0, the car behind at +0x11F8 (the car count of
// _GET_TRAIN_CARRIAGE_TRAILER_NUMBER walks both, GET_TRAIN_CARRIAGE walks
// +0x11F8). The walk below runs on whatever reads those links.
//
// Pure: no engine or native access; unit tested in ReplayCoreTests.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::trains
{
    constexpr std::size_t kMaxConsistCars = 64;   // a walk longer than this is a broken list
    constexpr std::size_t kMaxRecordedCars = 96;  // cars recorded at once, every consist
    constexpr int kFirstTrainType = 17;           // [car+0xBB0] of a train (IS_THIS_MODEL_A_TRAIN: 17, 18)
    constexpr int kLastTrainType = 19;            // the car count and the whistle take 17..19

    inline bool IsTrainVehicleType(int type) { return type >= kFirstTrainType && type <= kLastTrainType; }

    // One car's links as read from the engine (addresses, 0 when none).
    struct CarLinks
    {
        std::uint64_t address = 0;
        std::uint64_t forward = 0;  // +0x11F0: the car in front
        std::uint64_t backward = 0; // +0x11F8: the car behind
    };

    enum class WalkStatus : std::uint8_t
    {
        Ok = 0,
        Unreadable, // a car's links could not be read
        Cycle,      // a car seen twice
        TooLong,    // more than kMaxConsistCars
        Broken,     // a car's neighbour does not point back at it
    };

    inline const char* WalkStatusText(WalkStatus status)
    {
        switch (status)
        {
        case WalkStatus::Ok:
            return "ok";
        case WalkStatus::Unreadable:
            return "unreadable";
        case WalkStatus::Cycle:
            return "cycle";
        case WalkStatus::TooLong:
            return "too_long";
        default:
            return "broken";
        }
    }

    // The whole consist of the car at `start`, front first: forward links
    // up to the head, then backward links to the tail. `read(address,
    // links)` reads one car (false when it cannot). A neighbour whose own
    // link does not point back ends the walk there (Broken, the cars found
    // so far kept): the engine rebuilds the links when a car is added or
    // taken off, and a half-updated list is not followed further.
    template <typename Read>
    WalkStatus WalkConsist(std::uint64_t start, Read&& read, std::vector<CarLinks>& ordered)
    {
        ordered.clear();
        if (!start)
            return WalkStatus::Unreadable;
        CarLinks at;
        if (!read(start, at))
            return WalkStatus::Unreadable;
        at.address = start;
        // To the head.
        std::vector<std::uint64_t> seen{start};
        WalkStatus status = WalkStatus::Ok;
        while (at.forward)
        {
            if (std::find(seen.begin(), seen.end(), at.forward) != seen.end())
            {
                status = WalkStatus::Cycle;
                break;
            }
            if (seen.size() >= kMaxConsistCars)
            {
                status = WalkStatus::TooLong;
                break;
            }
            CarLinks front;
            if (!read(at.forward, front))
            {
                status = WalkStatus::Unreadable;
                break;
            }
            front.address = at.forward;
            if (front.backward != at.address)
            {
                status = WalkStatus::Broken;
                break;
            }
            seen.push_back(front.address);
            at = front;
        }
        // From the head (or the last good car) back to the tail.
        ordered.push_back(at);
        std::vector<std::uint64_t> listed{at.address};
        while (ordered.back().backward)
        {
            const CarLinks& last = ordered.back();
            if (std::find(listed.begin(), listed.end(), last.backward) != listed.end())
                return WalkStatus::Cycle;
            if (listed.size() >= kMaxConsistCars)
                return WalkStatus::TooLong;
            CarLinks back;
            if (!read(last.backward, back))
                return status == WalkStatus::Ok ? WalkStatus::Unreadable : status;
            back.address = last.backward;
            if (back.forward != last.address)
                return status == WalkStatus::Ok ? WalkStatus::Broken : status;
            listed.push_back(back.address);
            ordered.push_back(back);
        }
        return status;
    }

    // What the recorder does with a consist this scan. The whole consist
    // enters when any car is within the enter radius (of the player or the
    // camera) and leaves only when every car is past the exit radius or
    // gone: no half train in the clip.
    enum class ConsistAction : std::uint8_t { Ignore, Enter, Keep, Leave };

    inline ConsistAction DecideConsist(bool recorded, float nearestCarM, float enterRadiusM, float exitRadiusM,
        bool anyCarAlive)
    {
        if (!recorded)
            return anyCarAlive && nearestCarM <= enterRadiusM ? ConsistAction::Enter : ConsistAction::Ignore;
        return anyCarAlive && nearestCarM <= exitRadiusM ? ConsistAction::Keep : ConsistAction::Leave;
    }

    // Car flags (TrainCar::flags).
    constexpr std::uint8_t kCarHead = 1u << 0;      // no car in front (the engine of a normal train)
    constexpr std::uint8_t kCarTail = 1u << 1;      // no car behind
    constexpr std::uint8_t kCarMission = 1u << 2;   // a script's train (a mission entity when recorded)
    constexpr std::uint8_t kCarForward = 1u << 3;   // _GET_TRAIN_DIRECTION of the head car was true
    constexpr std::uint8_t kCarLinksRead = 1u << 4; // the consist came from the engine links (else the natives)

    // The clip's record of one car (section TRNC). Ids are packed replay
    // ids; 0 when there is none (or the neighbour is not in the clip).
    struct TrainCar
    {
        std::uint32_t id = 0;
        std::uint32_t head = 0;     // the consist's front car: the consist key
        std::uint32_t forward = 0;  // car in front
        std::uint32_t backward = 0; // car behind
        std::uint16_t index = 0;    // from the front, 0 = head
        std::uint16_t cars = 0;     // cars of the consist when it entered
        std::uint8_t flags = 0;
        std::uint8_t vehicleType = 0;  // [car+0xBB0] (17..19), 0 when not read
        std::int32_t track = -1;       // _GET_TRAIN_TRACK_FROM_TRAIN_VEHICLE of the head
        std::uint32_t propSet = 0;     // cargo (_GET_TRAIN_CARRIAGE_PROP_SET's model, else the vehicle prop set)
        std::uint32_t lightPropSet = 0; // lanterns
    };

    // The head car's speed along the track (m/s) and direction, sampled a
    // few times a second and kept when it changes. Evidence and the audio
    // of a train at rest.
    struct SpeedSample
    {
        std::uint32_t timeMs = 0;
        float speed = 0.0f;
        std::uint8_t forward = 0;
    };

    constexpr std::uint32_t kSpeedIntervalMs = 250;
    constexpr float kSpeedChange = 0.05f;        // m/s
    constexpr std::uint32_t kSpeedKeepMs = 2000; // a sample at least this often

    struct SpeedTrack
    {
        std::uint32_t head = 0;
        std::vector<SpeedSample> samples; // time ascending

        // Rate gated and kept on change; false when not stored.
        bool Append(std::uint32_t timeMs, float speed, bool forward)
        {
            if (!samples.empty())
            {
                const SpeedSample& last = samples.back();
                if (timeMs < last.timeMs + kSpeedIntervalMs)
                    return false;
                const bool same = std::fabs(speed - last.speed) < kSpeedChange && (last.forward != 0) == forward;
                if (same && timeMs < last.timeMs + kSpeedKeepMs)
                    return false;
            }
            samples.push_back({timeMs, speed, static_cast<std::uint8_t>(forward ? 1 : 0)});
            return true;
        }

        // Interpolated speed at timeMs (clamped); the direction of the
        // earlier sample. False when empty.
        bool At(std::uint32_t timeMs, float& speed, bool& forward) const
        {
            if (samples.empty())
                return false;
            if (timeMs <= samples.front().timeMs)
            {
                speed = samples.front().speed;
                forward = samples.front().forward != 0;
                return true;
            }
            for (std::size_t n = 1; n < samples.size(); ++n)
            {
                if (timeMs > samples[n].timeMs)
                    continue;
                const SpeedSample& a = samples[n - 1];
                const SpeedSample& b = samples[n];
                const float t = b.timeMs > a.timeMs
                    ? static_cast<float>(timeMs - a.timeMs) / static_cast<float>(b.timeMs - a.timeMs) : 1.0f;
                speed = a.speed + (b.speed - a.speed) * t;
                forward = a.forward != 0;
                return true;
            }
            speed = samples.back().speed;
            forward = samples.back().forward != 0;
            return true;
        }

        // Replay buffer: the samples before timeMs go but the last of them.
        std::size_t DropBefore(std::uint32_t timeMs)
        {
            std::size_t keep = 0;
            while (keep + 1 < samples.size() && samples[keep + 1].timeMs <= timeMs)
                ++keep;
            if (keep)
                samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(keep));
            return keep;
        }
    };

    struct TrainData
    {
        std::vector<TrainCar> cars;
        std::vector<SpeedTrack> speeds;

        bool Empty() const { return cars.empty() && speeds.empty(); }
        void Clear()
        {
            cars.clear();
            speeds.clear();
        }
        const TrainCar* Find(std::uint32_t id) const
        {
            for (const TrainCar& car : cars)
                if (car.id == id)
                    return &car;
            return nullptr;
        }
        SpeedTrack* SpeedOf(std::uint32_t head)
        {
            for (SpeedTrack& track : speeds)
                if (track.head == head)
                    return &track;
            return nullptr;
        }
        const SpeedTrack* SpeedOf(std::uint32_t head) const { return const_cast<TrainData*>(this)->SpeedOf(head); }
        std::size_t DropBefore(std::uint32_t timeMs)
        {
            std::size_t dropped = 0;
            for (SpeedTrack& track : speeds)
                dropped += track.DropBefore(timeMs);
            return dropped;
        }

        // A clip window keeps the cars `kept(id)` says; a neighbour or head
        // that was not kept reads 0 (its car is not in the window). The
        // speed samples go through `cut` (the window's sample cut).
        template <typename Kept, typename CutSamples>
        void CutInto(TrainData& out, Kept&& kept, CutSamples&& cut) const
        {
            out.Clear();
            for (const TrainCar& car : cars)
            {
                if (!kept(car.id))
                    continue;
                TrainCar copy = car;
                if (copy.head && !kept(copy.head))
                    copy.head = 0;
                if (copy.forward && !kept(copy.forward))
                    copy.forward = 0;
                if (copy.backward && !kept(copy.backward))
                    copy.backward = 0;
                out.cars.push_back(copy);
            }
            for (const SpeedTrack& track : speeds)
            {
                if (!kept(track.head))
                    continue;
                SpeedTrack copy;
                copy.head = track.head;
                copy.samples = cut(track.samples);
                if (!copy.samples.empty())
                    out.speeds.push_back(std::move(copy));
            }
        }
    };

    // Links between the recorded cars by id, from the walk's order: the
    // front car is the head, each car's forward and backward its
    // neighbours in the list, index its place from the front.
    inline void LinkConsist(const std::vector<std::uint32_t>& idsFrontFirst, std::vector<TrainCar>& cars)
    {
        const std::uint32_t head = idsFrontFirst.empty() ? 0u : idsFrontFirst.front();
        for (std::size_t n = 0; n < idsFrontFirst.size(); ++n)
        {
            TrainCar* car = nullptr;
            for (TrainCar& candidate : cars)
                if (candidate.id == idsFrontFirst[n])
                    car = &candidate;
            if (!car)
                continue;
            car->head = head;
            car->index = static_cast<std::uint16_t>(n);
            car->cars = static_cast<std::uint16_t>(idsFrontFirst.size());
            car->forward = n > 0 ? idsFrontFirst[n - 1] : 0u;
            car->backward = n + 1 < idsFrontFirst.size() ? idsFrontFirst[n + 1] : 0u;
            car->flags = static_cast<std::uint8_t>(car->flags & ~(kCarHead | kCarTail));
            if (n == 0)
                car->flags |= kCarHead;
            if (n + 1 == idsFrontFirst.size())
                car->flags |= kCarTail;
        }
    }

    // A rigid track freezes the wheels and rods. Their spin is in the car's
    // skeleton (24 to 143 bones resolve on every train car), so the cars
    // nearest the player or the camera get a pose record like an animated
    // object (the pelt, the drawers, the wagons): at most kPoseCars at once,
    // within kPoseRadiusM, in capture slots of their own (taken from the
    // weapons' spare slots, never from the bodies).
    constexpr std::uint32_t kPoseCars = 6;
    constexpr float kPoseRadiusM = 100.0f;
    constexpr std::uint32_t kPoseMinBones = 2;

    inline bool ShouldPoseCar(std::uint32_t bones, std::size_t posedCars, float distanceM)
    {
        return bones >= kPoseMinBones && posedCars < kPoseCars && distanceM <= kPoseRadiusM;
    }

    // The order the free places go in: the nearest car first.
    inline std::vector<std::size_t> PoseOrder(const std::vector<float>& distancesM)
    {
        std::vector<std::size_t> order(distancesM.size());
        for (std::size_t n = 0; n < order.size(); ++n)
            order[n] = n;
        std::stable_sort(order.begin(), order.end(),
            [&](std::size_t a, std::size_t b) { return distancesM[a] < distancesM[b]; });
        return order;
    }

    // Playback puppet of a car: a vehicle of the model first; an object of
    // the same model when the engine refuses the vehicle or does not leave
    // it where it is placed (the train system pulling it onto a track).
    // PuppetJudge decides from what the playback measures.
    enum class PuppetKind : std::uint8_t { Vehicle = 0, Object = 1 };

    struct PuppetJudge
    {
        static constexpr std::uint32_t kRefusalsForObject = 3; // CREATE_VEHICLE returning 0
        static constexpr std::uint32_t kDriftsForObject = 3;   // checks in a row off the placement
        static constexpr float kDriftM = 0.5f;

        std::uint32_t refusals = 0;
        std::uint32_t driftStreak = 0;
        std::uint32_t drifts = 0;
        float worstDriftM = 0.0f;

        // A create refused; true when the next try is an object.
        bool Refused()
        {
            ++refusals;
            return refusals >= kRefusalsForObject;
        }
        // Where the engine had the puppet against where it was placed the
        // tick before; true when the puppet should become an object.
        bool Drift(float meters)
        {
            worstDriftM = (std::max)(worstDriftM, meters);
            if (meters <= kDriftM)
            {
                driftStreak = 0;
                return false;
            }
            ++drifts;
            ++driftStreak;
            return driftStreak >= kDriftsForObject;
        }
    };
}
