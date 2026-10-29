#pragma once
// The result of the movement volume 0x31815C4 (foot audio, limb, foot, value,
// out) of each recorded ped's and horse's step, recorded (section FMOV) and
// given back to the puppet's step at the same clip time instead of the one the
// puppet computes.
//
// The puppet's own result can differ by about 2 dB (the subterm, the limb term
// 0x317B9D0 under +0x774 or the tunable 0xD1660BEE, is not identified). The
// recorded result already holds every term, so MovementTermCorrection does not
// add on top.
//
// Pure: the track, the matching of a puppet's step to the recorded one and the
// trimming of the recording ring (tests/foot_movement_tests.cpp).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::footmovement
{
    constexpr std::uint8_t kMaxFoot = 4;              // a horse's feet (a ped uses 2)
    constexpr std::size_t kMaxSamples = 400000;       // about an hour of 4 feet at a gallop
    constexpr float kMinResult = -60.0f;              // a dB term (the live results seen: -2 to +2.1)
    constexpr float kMaxResult = 24.0f;
    // A puppet step takes the recorded step of the same owner and foot nearest
    // in clip time within this window (a step lasts 250 to 700 ms per foot).
    constexpr std::uint32_t kMatchMs = 200;

    struct Sample
    {
        std::uint32_t owner = 0;  // packed replay id of the ped or horse
        std::uint32_t timeMs = 0;
        std::uint8_t foot = 0;
        float result = 0.0f;
    };
    constexpr std::size_t kSampleBytes = 4 + 4 + 1 + 4;

    inline bool ValidSample(const Sample& s)
    {
        return s.foot < kMaxFoot && std::isfinite(s.result) && s.result >= kMinResult && s.result <= kMaxResult;
    }

    struct Track
    {
        std::vector<Sample> samples; // time ascending

        void Clear() { samples.clear(); }
        bool Empty() const { return samples.empty(); }
        std::size_t ApproxBytes() const { return samples.capacity() * sizeof(Sample); }

        // A late time is clamped to the last one. False: invalid or the cap.
        bool Add(const Sample& s)
        {
            if (!ValidSample(s) || samples.size() >= kMaxSamples)
                return false;
            Sample copy = s;
            if (!samples.empty() && copy.timeMs < samples.back().timeMs)
                copy.timeMs = samples.back().timeMs;
            samples.push_back(copy);
            return true;
        }

        std::size_t DropBefore(std::uint32_t keepFromMs)
        {
            const auto first = std::lower_bound(samples.begin(), samples.end(), keepFromMs,
                [](const Sample& s, std::uint32_t t) { return s.timeMs < t; });
            const std::size_t count = static_cast<std::size_t>(first - samples.begin());
            samples.erase(samples.begin(), first);
            return count;
        }

        template <typename Kept>
        void CutInto(Track& out, std::uint32_t baseMs, std::uint32_t endMs, Kept kept) const
        {
            out.Clear();
            for (const Sample& s : samples)
            {
                if (s.timeMs > endMs)
                    break;
                if (s.timeMs < baseMs || !kept(s.owner))
                    continue;
                Sample copy = s;
                copy.timeMs = s.timeMs - baseMs;
                out.samples.push_back(copy);
            }
        }

        bool Valid() const
        {
            if (samples.size() > kMaxSamples)
                return false;
            for (std::size_t n = 0; n < samples.size(); ++n)
                if (!ValidSample(samples[n]) || (n && samples[n - 1].timeMs > samples[n].timeMs))
                    return false;
            return true;
        }
    };

    // The recorded result of owner's foot nearest to timeMs within kMatchMs.
    inline bool Nearest(const Track& track, std::uint32_t owner, std::uint8_t foot, std::uint32_t timeMs, float& out)
    {
        const std::uint32_t from = timeMs > kMatchMs ? timeMs - kMatchMs : 0;
        const auto first = std::lower_bound(track.samples.begin(), track.samples.end(), from,
            [](const Sample& s, std::uint32_t t) { return s.timeMs < t; });
        std::uint32_t best = kMatchMs + 1;
        bool found = false;
        for (auto it = first; it != track.samples.end() && it->timeMs <= timeMs + kMatchMs; ++it)
        {
            if (it->owner != owner || it->foot != foot)
                continue;
            const std::uint32_t d = it->timeMs > timeMs ? it->timeMs - timeMs : timeMs - it->timeMs;
            if (d < best)
            {
                best = d;
                out = it->result;
                found = true;
            }
        }
        return found;
    }

    // Whether the playback gives a puppet foot the recorded result: the key on and
    // a recorded step matched. Without one (a clip without FMOV) the puppet keeps
    // the engine's result and the correction of MovementTermCorrection.
    inline bool UseRecorded(bool replayOn, bool matched) { return replayOn && matched; }
}
