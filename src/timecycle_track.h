#pragma once

// The script timecycle modifier and the active weather variations of the clip
// over time. The timecycle modifier keys (script modifier, transition) and the
// weather variation are recorded and played back as data; a mission's look
// ("WINTER2_mountainWind", "blizzard_winter2") would otherwise be whatever the
// session has when the clip plays.
//
// Engine model (build 1491.50; the engine side is timecycle_state.cpp):
//   timecycle object T: T+0x2770 modifier index, +0x2774 strength,
//     +0x2778 transition index, +0x277C transition progress, +0x2780 speed
//     (progress per second), +0x2784 fade-out byte. The per frame update
//     (0xF22B60) adds speed * frame time to the progress of a transition and
//     finishes it (the target becomes the modifier) when progress reaches the
//     strength; with the fade-out byte it takes speed * frame time off and
//     clears the modifier at 0.
//   An index is a position in T's modifier pointer array (T+0x40, count at
//     T+0x48). SET_TIMECYCLE_MODIFIER finds it from the name hash through a
//     sorted map of (hash, pointer) at T+0x50 (data at +0x58, count at
//     +0x60) and the position of that pointer in the array (0x2B2ED78).
//   Weather type i at W + i * 0x1E0: +0x10 its hash, +0x14 the key of the
//     active variation (the type's own hash, or 0, when none), the variation
//     table at +0x1C8 (entries of 0x44 bytes, key first), count at +0x1D0.
//
// Keys are stored, never indexes (an index is a session's array position; the
// index is kept only to compare in the log). Pure data and rules here.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay
{
    namespace tctrack
    {
        // A weather type with a variation other than its own (the type's hash
        // and the variation's key). At most this many per sample (the
        // missions set one or two).
        constexpr std::size_t kMaxVariations = 16;
    }

    struct WeatherVariation
    {
        std::uint32_t type = 0;
        std::uint32_t variation = 0;
    };

    struct TimecycleSample
    {
        std::uint32_t timeMs = 0;
        std::uint32_t modifierKey = 0;   // 0: no script modifier
        std::int32_t modifierIndex = -1; // the session's index when recorded (log only)
        float strength = 1.0f;
        std::uint32_t transitionKey = 0; // 0: no transition
        std::int32_t transitionIndex = -1;
        float progress = 0.0f; // transition progress, or the strength left while fading out
        float speed = 0.0f;    // progress per second (recorded; playback drives the progress itself)
        std::uint8_t fadeOut = 0;
        std::uint8_t variationCount = 0;
        WeatherVariation variations[tctrack::kMaxVariations] = {};
    };

    namespace tctrack
    {
        // 10 Hz poll; a sample is kept when something moved (a transition
        // moves every poll, a held modifier is one sample).
        constexpr std::uint32_t kSampleMs = 100;
        constexpr float kEpsilon = 0.0005f;
        // CLEAR_TIMECYCLE_MODIFIER's strength (the .rdata 1.0 at 0x330C7A8).
        constexpr float kDefaultStrength = 1.0f;

        // No variation: the key is the type's own hash (the immediate clear,
        // 0x72C2E8, copies +0x10 to +0x14) or 0 (a type never touched).
        constexpr bool IsDefaultVariation(std::uint32_t type, std::uint32_t key)
        {
            return key == 0 || key == type;
        }

        inline bool Finite(const TimecycleSample& s)
        {
            return std::isfinite(s.strength) && std::isfinite(s.progress) && std::isfinite(s.speed);
        }

        inline bool Valid(const TimecycleSample& s)
        {
            if (!Finite(s) || s.variationCount > kMaxVariations || s.modifierIndex < -1 || s.transitionIndex < -1)
                return false;
            for (std::size_t n = 0; n < s.variationCount; ++n)
            {
                const WeatherVariation& v = s.variations[n];
                if (!v.type || IsDefaultVariation(v.type, v.variation))
                    return false;
                if (n && s.variations[n - 1].type >= v.type) // sorted, one per type
                    return false;
            }
            return true;
        }

        // Adds a type's variation to a sample being read (default ones are
        // not stored); false when the sample is full. Kept sorted by type.
        inline bool AddVariation(TimecycleSample& s, std::uint32_t type, std::uint32_t key)
        {
            if (!type || IsDefaultVariation(type, key))
                return true;
            for (std::size_t n = 0; n < s.variationCount; ++n)
                if (s.variations[n].type == type)
                {
                    s.variations[n].variation = key;
                    return true;
                }
            if (s.variationCount >= kMaxVariations)
                return false;
            std::size_t at = s.variationCount;
            while (at > 0 && s.variations[at - 1].type > type)
            {
                s.variations[at] = s.variations[at - 1];
                --at;
            }
            s.variations[at] = WeatherVariation{type, key};
            ++s.variationCount;
            return true;
        }

        // The variation a sample wants for a type (0: the default).
        inline std::uint32_t VariationOf(const TimecycleSample& s, std::uint32_t type)
        {
            for (std::size_t n = 0; n < s.variationCount; ++n)
                if (s.variations[n].type == type)
                    return s.variations[n].variation;
            return 0;
        }

        inline bool SameVariations(const TimecycleSample& a, const TimecycleSample& b)
        {
            if (a.variationCount != b.variationCount)
                return false;
            for (std::size_t n = 0; n < a.variationCount; ++n)
                if (a.variations[n].type != b.variations[n].type || a.variations[n].variation != b.variations[n].variation)
                    return false;
            return true;
        }

        // The same modifier, transition, fade and variations: the floats in
        // between may be interpolated.
        inline bool SameKeys(const TimecycleSample& a, const TimecycleSample& b)
        {
            return a.modifierKey == b.modifierKey && a.transitionKey == b.transitionKey && a.fadeOut == b.fadeOut &&
                SameVariations(a, b);
        }

        inline bool Changed(const TimecycleSample& last, const TimecycleSample& now)
        {
            if (!SameKeys(last, now) || last.modifierIndex != now.modifierIndex || last.transitionIndex != now.transitionIndex)
                return true;
            return std::fabs(last.strength - now.strength) >= kEpsilon || std::fabs(last.progress - now.progress) >= kEpsilon ||
                std::fabs(last.speed - now.speed) >= kEpsilon;
        }

        // The recording side: every poll goes through Poll. A change keeps the
        // sample and, when the state had held since an older kept sample, the
        // last unchanged poll before it, so the interpolation does not stretch
        // a ramp back over the time the state held.
        struct Recorder
        {
            TimecycleSample held;
            bool heldValid = false;

            void Reset() { heldValid = false; }

            // True when the sample was kept.
            bool Poll(std::vector<TimecycleSample>& track, const TimecycleSample& sample)
            {
                if (!Valid(sample))
                    return false;
                if (track.empty())
                {
                    track.push_back(sample);
                    heldValid = false;
                    return true;
                }
                if (sample.timeMs < track.back().timeMs)
                    return false;
                if (!Changed(track.back(), sample))
                {
                    held = sample;
                    heldValid = true;
                    return false;
                }
                if (heldValid && held.timeMs > track.back().timeMs && held.timeMs < sample.timeMs)
                    track.push_back(held);
                heldValid = false;
                if (sample.timeMs == track.back().timeMs)
                    track.back() = sample;
                else
                    track.push_back(sample);
                return true;
            }

            // The state at the stop is always the last sample.
            bool Finish(std::vector<TimecycleSample>& track, const TimecycleSample& sample)
            {
                if (Poll(track, sample))
                    return true;
                if (!Valid(sample) || track.empty() || sample.timeMs <= track.back().timeMs)
                    return false;
                track.push_back(sample);
                heldValid = false;
                return true;
            }
        };

        // The state at timeMs. The keys, indexes, fade and variations are the
        // earlier sample's; strength and progress are interpolated when the
        // next sample has the same keys (a transition between two polls).
        // Before the first sample: the first; after the last: the last.
        inline bool At(const std::vector<TimecycleSample>& track, std::uint32_t timeMs, TimecycleSample& out)
        {
            if (track.empty())
                return false;
            const auto after = std::upper_bound(track.begin(), track.end(), timeMs,
                [](std::uint32_t t, const TimecycleSample& s) { return t < s.timeMs; });
            if (after == track.begin())
            {
                out = track.front();
                out.timeMs = timeMs;
                return true;
            }
            const TimecycleSample& a = *(after - 1);
            out = a;
            out.timeMs = timeMs;
            if (after == track.end())
                return true;
            const TimecycleSample& b = *after;
            const std::uint32_t span = b.timeMs - a.timeMs;
            if (span == 0 || !SameKeys(a, b))
                return true;
            const float t = static_cast<float>(timeMs - a.timeMs) / static_cast<float>(span);
            out.strength = a.strength + (b.strength - a.strength) * t;
            out.progress = a.progress + (b.progress - a.progress) * t;
            return true;
        }

        // Buffer trim and clip cut, as skytrack::Cut: the state at baseMs
        // first (at 0), the rest shifted by -baseMs, those after endMs dropped.
        inline std::vector<TimecycleSample> Cut(const std::vector<TimecycleSample>& in, std::uint32_t baseMs, std::uint32_t endMs)
        {
            std::vector<TimecycleSample> out;
            TimecycleSample atBase;
            if (At(in, baseMs, atBase))
            {
                atBase.timeMs = 0;
                out.push_back(atBase);
            }
            for (const TimecycleSample& sample : in)
            {
                if (sample.timeMs <= baseMs || sample.timeMs > endMs)
                    continue;
                TimecycleSample shifted = sample;
                shifted.timeMs = sample.timeMs - baseMs;
                out.push_back(shifted);
            }
            return out;
        }

        // The ring buffer's trim keeps times as they are: the samples before
        // keepFrom go, the one in force there stays.
        inline std::size_t DropBefore(std::vector<TimecycleSample>& track, std::uint32_t keepFrom)
        {
            std::size_t keep = 0;
            while (keep + 1 < track.size() && track[keep + 1].timeMs <= keepFrom)
                ++keep;
            if (keep)
                track.erase(track.begin(), track.begin() + static_cast<std::ptrdiff_t>(keep));
            return keep;
        }

        // ---- The engine's tables, as copied by timecycle_state.cpp ----

        // One entry of the modifier map at T+0x50 (16 bytes in the engine:
        // the hash, padding, the modifier pointer).
        struct MapEntry
        {
            std::uint32_t key = 0;
            std::uint64_t pointer = 0;
        };

        // index -> key: the pointer at that position of the array, then the
        // map entry holding that pointer. 0 when out of range or not in the map.
        inline std::uint32_t KeyOfIndex(const std::vector<std::uint64_t>& pointers, const std::vector<MapEntry>& map, std::int32_t index)
        {
            if (index < 0 || static_cast<std::size_t>(index) >= pointers.size() || !pointers[static_cast<std::size_t>(index)])
                return 0;
            const std::uint64_t pointer = pointers[static_cast<std::size_t>(index)];
            for (const MapEntry& e : map)
                if (e.pointer == pointer)
                    return e.key;
            return 0;
        }

        // key -> index, as 0x2B2ED78 after its alias step: the map entry of
        // that key, then the position of its pointer in the array. -1 when
        // the key is 0 or unknown to this session.
        inline std::int32_t IndexOfKey(const std::vector<std::uint64_t>& pointers, const std::vector<MapEntry>& map, std::uint32_t key)
        {
            if (!key)
                return -1;
            std::uint64_t pointer = 0;
            for (const MapEntry& e : map)
                if (e.key == key)
                {
                    pointer = e.pointer;
                    break;
                }
            if (!pointer)
                return -1;
            for (std::size_t n = 0; n < pointers.size() && n < 0x7FFFFFFF; ++n)
                if (pointers[n] == pointer)
                    return static_cast<std::int32_t>(n);
            return -1;
        }

        // The variation index of a key in a type's table (the keys of its
        // 0x44-byte entries, in order), as 0x739DB4. -1 when absent.
        inline std::int32_t VariationIndex(const std::vector<std::uint32_t>& keys, std::uint32_t key)
        {
            for (std::size_t n = 0; n < keys.size() && n < 0x7FFFFFFF; ++n)
                if (keys[n] == key)
                    return static_cast<std::int32_t>(n);
            return -1;
        }

        // What playback does for one type: nothing, the immediate clear
        // (0x757BEC with index -1 and the immediate byte: +0x14 = +0x10), or a
        // set by index.
        enum class VariationStep : std::uint8_t
        {
            None,
            Clear,
            Set,
            Missing, // the wanted key is not in this session's table
        };

        inline VariationStep PlanVariation(std::uint32_t type, std::uint32_t live, std::uint32_t wanted, std::int32_t wantedIndex)
        {
            const bool liveDefault = IsDefaultVariation(type, live);
            if (IsDefaultVariation(type, wanted))
                return liveDefault ? VariationStep::None : VariationStep::Clear;
            if (live == wanted)
                return VariationStep::None;
            return wantedIndex < 0 ? VariationStep::Missing : VariationStep::Set;
        }

        // The six modifier fields as written to T+0x2770.
        struct ModifierFields
        {
            std::int32_t index = -1;
            float strength = kDefaultStrength;
            std::int32_t transitionIndex = -1;
            float progress = 0.0f;
            float speed = 0.0f;
            std::uint8_t fadeOut = 0;
        };

        // Playback's write for the state at the playhead with the indexes of
        // its keys in this session (-1 when unknown). The speed is 0: the
        // playhead drives the progress, so the engine neither moves it a
        // frame on nor finishes the transition or the fade by itself (pause,
        // seek and export draw the same look). A modifier the session does
        // not know is written as none; so is a transition (and the fade then
        // has nothing to fade).
        inline ModifierFields PlanModifier(const TimecycleSample& at, std::int32_t modifierIndex, std::int32_t transitionIndex,
            bool* unresolved = nullptr)
        {
            ModifierFields f;
            bool missing = false;
            if (at.modifierKey)
            {
                if (modifierIndex >= 0)
                    f.index = modifierIndex;
                else
                    missing = true;
            }
            if (at.transitionKey)
            {
                if (transitionIndex >= 0)
                    f.transitionIndex = transitionIndex;
                else
                    missing = true;
            }
            f.strength = std::isfinite(at.strength) ? at.strength : kDefaultStrength;
            const bool transition = f.transitionIndex >= 0;
            const bool fading = !transition && f.index >= 0 && at.fadeOut;
            f.progress = transition || fading ? (std::isfinite(at.progress) ? std::max(0.0f, at.progress) : 0.0f) : 0.0f;
            // A transition must stay short of the strength (the engine would
            // finish it this frame); a fade above 0 (it would clear it).
            if (transition && f.progress >= f.strength)
                f.progress = std::max(0.0f, f.strength - kEpsilon);
            if (fading && f.progress <= 0.0f)
                f.progress = kEpsilon;
            f.fadeOut = fading ? 1 : 0;
            f.speed = 0.0f;
            if (unresolved)
                *unresolved = missing;
            return f;
        }
    }
}
