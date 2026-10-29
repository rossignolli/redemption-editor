#pragma once

// The open ratio of every door of a recorded vehicle or train car, as a
// state track (pure, unit tested in ReplayCoreTests).
//
// Only the ratio is recorded (the engine draws the door bone from it, see
// vehicle_doors_engine.h), one sample per change, per script door index (the
// index SET_VEHICLE_DOOR_* takes); without it a train car's sliding door
// plays shut. One more channel holds the train car's door latch byte
// (kLatchChannel).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::vehdoors
{
    constexpr int kScriptDoors = 10;            // VEH_EXT_DOOR_DSIDE_F .. PSIDE_R
    constexpr std::uint8_t kLatchChannel = 15;  // the train car byte _0x762FDC4C19E5A981 sets
    constexpr int kChannels = 16;
    constexpr float kStoreStep = 0.01f;         // a ratio change stored
    constexpr std::uint32_t kSampleMs = 50;     // reads per vehicle, at most
    constexpr float kMatchTolerance = 0.05f;    // played against recorded
    constexpr float kDriveTolerance = 0.005f;   // below it the puppet's door is left alone
    constexpr std::uint32_t kSettleMs = 1500;   // a moving door may lag this long before a mismatch

    struct DoorSample
    {
        std::uint32_t timeMs = 0;
        std::uint8_t door = 0; // script door index, or kLatchChannel
        float ratio = 0.0f;    // 0 shut .. 1 open (the latch: 0 or 1)
    };

    struct DoorTrack
    {
        std::uint32_t id = 0;            // the vehicle's packed replay id
        std::vector<DoorSample> samples; // time ascending
        // Recording only.
        std::uint32_t lastCheckMs = 0;
        bool checked = false;
        std::uint16_t readMask = 0;                  // channels read at least once
        std::array<float, kChannels> lastRead{};     // the previous read per channel
        std::array<std::uint32_t, kChannels> lastReadMs{};
    };

    // The stored value: clamped to 0..1 (the engine clamps a car door's
    // ratio the same way) and rounded to a thousandth.
    inline float Quantize(float ratio)
    {
        if (!(ratio > 0.0f))
            return 0.0f;
        if (ratio >= 1.0f)
            return 1.0f;
        return std::round(ratio * 1000.0f) / 1000.0f;
    }

    inline const DoorSample* LastStored(const DoorTrack& track, std::uint8_t door)
    {
        for (auto it = track.samples.rbegin(); it != track.samples.rend(); ++it)
            if (it->door == door)
                return &*it;
        return nullptr;
    }

    // One read of one channel. The first read is always stored (the state
    // the vehicle entered with); after it a change of kStoreStep or more,
    // or a reach of the ends (0 or 1). A door that starts moving after a
    // rest gets its last rest read stored first, so the interpolation of
    // the playback starts where the door left (a sample per change only).
    // True when something was stored.
    inline bool Observe(DoorTrack& track, std::uint32_t timeMs, std::uint8_t door, float ratio)
    {
        if (door >= kChannels)
            return false;
        const float value = Quantize(ratio);
        const std::uint16_t bit = static_cast<std::uint16_t>(1u << door);
        const bool readBefore = (track.readMask & bit) != 0;
        const float previous = track.lastRead[door];
        const std::uint32_t previousMs = track.lastReadMs[door];
        track.readMask = static_cast<std::uint16_t>(track.readMask | bit);
        track.lastRead[door] = value;
        track.lastReadMs[door] = timeMs;
        const DoorSample* stored = LastStored(track, door);
        if (!stored)
        {
            track.samples.push_back({timeMs, door, value});
            return true;
        }
        const float delta = std::fabs(value - stored->ratio);
        const bool reachedEnd = (value == 0.0f || value == 1.0f) && value != stored->ratio;
        if (delta < kStoreStep && !reachedEnd)
            return false;
        if (readBefore && previousMs > stored->timeMs && previousMs < timeMs)
            track.samples.push_back({previousMs, door, previous}); // where it left from
        track.samples.push_back({timeMs, door, value});
        return true;
    }

    // The recorded value of a channel at timeMs: interpolated between its
    // samples around it, the first one before it, the last one after it.
    // False when the channel has no sample.
    inline bool RatioAt(const DoorTrack& track, std::uint8_t door, std::uint32_t timeMs, float& out)
    {
        const DoorSample* before = nullptr;
        const DoorSample* after = nullptr;
        for (const DoorSample& sample : track.samples)
        {
            if (sample.door != door)
                continue;
            if (sample.timeMs <= timeMs)
                before = &sample;
            else
            {
                after = &sample;
                break;
            }
        }
        if (!before && !after)
            return false;
        if (!before)
        {
            out = after->ratio;
            return true;
        }
        if (!after || door == kLatchChannel || after->timeMs == before->timeMs)
        {
            out = before->ratio;
            return true;
        }
        const float t = static_cast<float>(timeMs - before->timeMs) / static_cast<float>(after->timeMs - before->timeMs);
        out = before->ratio + (after->ratio - before->ratio) * t;
        return true;
    }

    // The channels with a sample (bit n: channel n).
    inline std::uint16_t Channels(const DoorTrack& track)
    {
        std::uint16_t mask = 0;
        for (const DoorSample& sample : track.samples)
            if (sample.door < kChannels)
                mask = static_cast<std::uint16_t>(mask | (1u << sample.door));
        return mask;
    }

    // Whether any door channel ever opens (a ratio above the store step).
    inline bool EverOpen(const DoorTrack& track)
    {
        for (const DoorSample& sample : track.samples)
            if (sample.door < kScriptDoors && sample.ratio >= kStoreStep)
                return true;
        return false;
    }

    // Replay buffer: the samples at or before keepFromMs go but the last of
    // each channel (the state in force at the new start).
    inline std::size_t DropBefore(DoorTrack& track, std::uint32_t keepFromMs)
    {
        std::array<int, kChannels> last;
        last.fill(-1);
        for (std::size_t n = 0; n < track.samples.size(); ++n)
            if (track.samples[n].timeMs <= keepFromMs && track.samples[n].door < kChannels)
                last[track.samples[n].door] = static_cast<int>(n);
        std::vector<DoorSample> kept;
        kept.reserve(track.samples.size());
        for (std::size_t n = 0; n < track.samples.size(); ++n)
        {
            const DoorSample& sample = track.samples[n];
            if (sample.timeMs > keepFromMs || (sample.door < kChannels && last[sample.door] == static_cast<int>(n)))
                kept.push_back(sample);
        }
        const std::size_t dropped = track.samples.size() - kept.size();
        track.samples.swap(kept);
        return dropped;
    }

    // Playback: the puppet's door is driven when it is not where the record
    // has it (or cannot be read).
    inline bool NeedsDrive(float wanted, float got, bool readable)
    {
        return !readable || std::fabs(wanted - got) > kDriveTolerance;
    }

    enum class Verdict : std::uint8_t { Match, Settling, Mismatch, Unread };

    // The readback after the drive: a door off the record by more than
    // kMatchTolerance is a mismatch once the record has held its value (or
    // the door has been driven) for kSettleMs.
    inline Verdict Judge(float wanted, float got, bool readable, std::uint32_t sinceChangeMs)
    {
        if (!readable)
            return Verdict::Unread;
        if (std::fabs(wanted - got) <= kMatchTolerance)
            return Verdict::Match;
        return sinceChangeMs < kSettleMs ? Verdict::Settling : Verdict::Mismatch;
    }
}
