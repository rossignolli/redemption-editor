#pragma once

// The editor's environment settings of a clip. Each one is "Recorded" (what the
// clip recorded plays: the clock and weather track, ENVT, and the cloud track,
// SKYT) or an override the user picked in the editor. Overrides only change
// what the replay applies at the playhead (ApplyClipEnvironment and
// ApplyClipSky in script.cpp, the same natives as the recorded state), so the
// export, which plays the same state, renders them too, and EndClipWorld gives
// the world back as before (nothing leaks into gameplay).
//
// One set per clip, as a replay editor keeps its weather and time overrides
// per playback. A gallery clip keeps it in its own file (section ENVO), a
// project clip in the project (the clip entry), like the markers.
//
// Not here: the sun's direction in RDR2 is computed from the clock by the sky
// object's astronomy (latitude, longitude, day of the year are its own fields,
// no native sets them), and ENABLE_MOON_CYCLE_OVERRIDE writes a value no code
// of the build reads. Both would need writes into an engine object the replay
// does not own, which the access contract leaves to the user.
//
// Pure data and rules, unit tested in tests/env_override_tests.cpp; the
// editor includes it for the weather names.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "i18n.h" // the weather names in the player's language

namespace replay
{
    struct EnvironmentOverride
    {
        // The clock at the clip's start, in minutes since midnight; -1 is
        // Recorded. The recorded flow of the clock keeps going from it (the
        // clip's own pace, shifted), so a clip whose light moved still moves.
        // kClockLive leaves the clock to the game (and to other mods, a time
        // changer like Rampage): the replay does not set it.
        static constexpr std::int32_t kClockLive = -2;
        std::int32_t clockMinutes = -1;
        // A weather type hash; 0 is Recorded; kWeatherLive leaves the weather
        // to the game, as kClockLive the clock.
        static constexpr std::uint32_t kWeatherLive = 0xFFFFFFFFu;
        std::uint32_t weather = 0;
        // Seconds from the clip's start over which the recorded weather
        // blends into the override (SET_CURR_WEATHER_STATE's mix); 0 a cut.
        std::uint16_t blendS = 0;
        // 0: the recorded clouds (the SKYT track pinned at the playhead);
        // 1: live, the engine moves them as in gameplay.
        std::uint8_t cloudsLive = 0;
        std::uint8_t reserved = 0;
        // No sun or moon override: the experimental sun and moon rows were
        // cut from 1.0. The bytes they had in ENVO (kBytes) are still read,
        // and ignored, so a clip saved with them opens; the sky plays as
        // recorded (ENVT, SKYT, MOON).
        bool ClockLive() const { return clockMinutes == kClockLive; }
        bool WeatherLive() const { return weather == kWeatherLive; }
        bool Any() const { return clockMinutes != -1 || weather != 0 || cloudsLive != 0; }
        bool operator==(const EnvironmentOverride& o) const
        {
            return clockMinutes == o.clockMinutes && weather == o.weather && blendS == o.blendS && cloudsLive == o.cloudsLive;
        }
        bool operator!=(const EnvironmentOverride& o) const { return !(*this == o); }
    };

    namespace envoverride
    {
        constexpr std::uint32_t kDaySeconds = 24u * 60u * 60u;
        constexpr std::int32_t kDayMinutes = 24 * 60;
        constexpr std::int32_t kClockStepMinutes = 15;

        // The game's weather types (the names SET_WEATHER_TYPE and the
        // scripts use) and what the editor shows. The clip stores the hash.
        struct WeatherName
        {
            const char* game;
            const char* key;   // the label's lang key
            const char* label; // English, as a clip's details store it
        };
        constexpr WeatherName kWeathers[] = {
            {"SUNNY", TR_NOOP("weather.sunny", "Sunny")},
            {"HIGHPRESSURE", TR_NOOP("weather.highpressure", "Clear")},
            {"CLOUDS", TR_NOOP("weather.clouds", "Cloudy")},
            {"OVERCAST", TR_NOOP("weather.overcast", "Overcast")},
            {"OVERCASTDARK", TR_NOOP("weather.overcastdark", "Dark overcast")},
            {"DRIZZLE", TR_NOOP("weather.drizzle", "Drizzle")},
            {"RAIN", TR_NOOP("weather.rain", "Rain")},
            {"SHOWER", TR_NOOP("weather.shower", "Showers")},
            {"THUNDER", TR_NOOP("weather.thunder", "Thunder")},
            {"THUNDERSTORM", TR_NOOP("weather.thunderstorm", "Thunderstorm")},
            {"HURRICANE", TR_NOOP("weather.hurricane", "Hurricane")},
            {"FOG", TR_NOOP("weather.fog", "Fog")},
            {"MISTY", TR_NOOP("weather.misty", "Mist")},
            {"SANDSTORM", TR_NOOP("weather.sandstorm", "Sandstorm")},
            {"SLEET", TR_NOOP("weather.sleet", "Sleet")},
            {"HAIL", TR_NOOP("weather.hail", "Hail")},
            {"SNOWLIGHT", TR_NOOP("weather.snowlight", "Light snow")},
            {"SNOW", TR_NOOP("weather.snow", "Snow")},
            {"SNOWCLEARING", TR_NOOP("weather.snowclearing", "Snow clearing")},
            {"BLIZZARD", TR_NOOP("weather.blizzard", "Blizzard")},
            {"GROUNDBLIZZARD", TR_NOOP("weather.groundblizzard", "Ground blizzard")},
            {"WHITEOUT", TR_NOOP("weather.whiteout", "Whiteout")}};
        constexpr std::uint32_t kWeatherCount = sizeof(kWeathers) / sizeof(kWeathers[0]);

        // The blend choices, seconds (index 0 is a cut).
        constexpr std::uint16_t kBlendSeconds[] = {0, 2, 5, 10, 20, 30};
        constexpr std::uint32_t kBlendCount = sizeof(kBlendSeconds) / sizeof(kBlendSeconds[0]);

        // GET_HASH_KEY: Jenkins one-at-a-time of the lower-case text.
        constexpr std::uint32_t Joaat(const char* text)
        {
            std::uint32_t hash = 0;
            for (; *text; ++text)
            {
                char c = *text;
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
                hash += static_cast<std::uint8_t>(c);
                hash += hash << 10;
                hash ^= hash >> 6;
            }
            hash += hash << 3;
            hash ^= hash >> 11;
            hash += hash << 15;
            return hash;
        }

        // A weather's name in the player's language: an English label of
        // kWeathers (as a clip stores it), any other text as it is.
        inline const char* WeatherDisplay(const char* label)
        {
            for (const WeatherName& weather : kWeathers)
                if (label && std::strcmp(label, weather.label) == 0)
                    return TR(weather.key, weather.label);
            return label;
        }

        inline std::uint32_t WeatherHash(std::uint32_t index) { return index < kWeatherCount ? Joaat(kWeathers[index].game) : 0; }

        // The list index of a hash, -1 when it is not in the list.
        inline int WeatherIndex(std::uint32_t hash)
        {
            for (std::uint32_t n = 0; n < kWeatherCount; ++n)
                if (WeatherHash(n) == hash)
                    return static_cast<int>(n);
            return -1;
        }

        inline int BlendIndex(std::uint16_t seconds)
        {
            for (std::uint32_t n = 0; n < kBlendCount; ++n)
                if (kBlendSeconds[n] == seconds)
                    return static_cast<int>(n);
            return -1;
        }

        constexpr std::int32_t kClockLive = EnvironmentOverride::kClockLive;
        constexpr std::uint32_t kWeatherLive = EnvironmentOverride::kWeatherLive;

        // A value the file or the project may hold.
        inline bool Valid(const EnvironmentOverride& o)
        {
            return o.clockMinutes >= kClockLive && o.clockMinutes < kDayMinutes &&
                (o.weather == 0 || o.weather == kWeatherLive || WeatherIndex(o.weather) >= 0) &&
                BlendIndex(o.blendS) >= 0 && o.cloudsLive <= 1;
        }

        // ---- The editor's steps (left -1, right +1). Each cycles with
        // Recorded as its first value.

        // Out of Recorded going right: the recorded start rounded down to
        // the step, so the first press stays near the clip's own light.
        // Then 15 minutes a step; past 00:00 going left or 23:45 going right
        // it is Recorded again. Left of Recorded is Live (the game's clock),
        // the end of the row: right from it is Recorded again.
        inline std::int32_t StepClock(std::int32_t clockMinutes, std::uint32_t recordedStartS, int step)
        {
            if (step == 0)
                return clockMinutes;
            if (clockMinutes == kClockLive)
                return step > 0 ? -1 : kClockLive;
            if (clockMinutes < 0 || clockMinutes >= kDayMinutes)
            {
                if (step < 0)
                    return kClockLive;
                const std::int32_t start = static_cast<std::int32_t>(recordedStartS % kDaySeconds / 60u);
                return start / kClockStepMinutes * kClockStepMinutes;
            }
            const std::int32_t down = clockMinutes / kClockStepMinutes * kClockStepMinutes;
            std::int32_t next = 0;
            if (step > 0)
                next = down + kClockStepMinutes;
            else
                next = down == clockMinutes ? down - kClockStepMinutes : down;
            return next < 0 || next >= kDayMinutes ? -1 : next;
        }

        // Recorded -> the list in order -> Live (the game's weather) ->
        // Recorded.
        inline std::uint32_t StepWeather(std::uint32_t weather, int step)
        {
            if (step == 0)
                return weather;
            const int count = static_cast<int>(kWeatherCount);
            // Position on the ring: 0 Recorded, 1..count the list, count + 1 Live.
            const int ring = count + 2;
            int at = weather == 0 ? 0 : weather == kWeatherLive ? count + 1 : WeatherIndex(weather) + 1;
            if (at < 0)
                at = 0;
            at = ((at + (step > 0 ? 1 : -1)) % ring + ring) % ring;
            if (at == 0)
                return 0u;
            return at == count + 1 ? kWeatherLive : WeatherHash(static_cast<std::uint32_t>(at - 1));
        }

        inline std::uint16_t StepBlend(std::uint16_t seconds, int step)
        {
            if (step == 0)
                return seconds;
            int at = BlendIndex(seconds);
            if (at < 0)
                at = 0;
            const int count = static_cast<int>(kBlendCount);
            at = ((at + (step > 0 ? 1 : -1)) % count + count) % count;
            return kBlendSeconds[at];
        }

        // ---- At the playhead.

        // The recorded state there, for Resolve (EnvironmentSample's fields
        // that the replay applies).
        struct Recorded
        {
            std::uint32_t clockS = 0;      // the recorded clock at the playhead
            std::uint32_t startClockS = 0; // the recorded clock at the clip's start (originMs)
            std::uint32_t weatherFrom = 0;
            std::uint32_t weatherTo = 0;
            float weatherMix = 0.0f;
        };

        struct Applied
        {
            std::uint32_t clockS = 0;
            std::uint32_t weatherFrom = 0;
            std::uint32_t weatherTo = 0;
            float weatherMix = 0.0f;
            bool pinClouds = true;
            bool clockOverridden = false;
            bool weatherOverridden = false;
            // Live: the replay leaves the clock or the weather to the game
            // (the fields above then keep the recorded state, for the log).
            bool clockLive = false;
            bool weatherLive = false;
        };

        // What the replay applies at `playheadMs`. `originMs` is where the
        // clip starts for the user (0 for a gallery clip, the trim's in for a
        // project clip): the override clock is the clock there and the blend
        // starts there. Nothing depends on the ticks before: the same
        // playhead gives the same state (scrub, jump, export).
        inline Applied Resolve(const Recorded& recorded, const EnvironmentOverride& o, std::uint64_t playheadMs, std::uint64_t originMs)
        {
            Applied out;
            out.clockS = recorded.clockS % kDaySeconds;
            out.weatherFrom = recorded.weatherFrom;
            out.weatherTo = recorded.weatherTo;
            out.weatherMix = recorded.weatherMix;
            out.pinClouds = o.cloudsLive == 0;
            out.clockLive = o.ClockLive();
            out.weatherLive = o.WeatherLive();
            if (o.clockMinutes >= 0 && o.clockMinutes < kDayMinutes)
            {
                // The recorded flow since the start, forward across midnight.
                const std::uint32_t moved = (recorded.clockS % kDaySeconds + kDaySeconds - recorded.startClockS % kDaySeconds) % kDaySeconds;
                out.clockS = (static_cast<std::uint32_t>(o.clockMinutes) * 60u + moved) % kDaySeconds;
                out.clockOverridden = true;
            }
            if (o.weather != 0 && !o.WeatherLive())
            {
                out.weatherOverridden = true;
                const std::uint64_t since = playheadMs > originMs ? playheadMs - originMs : 0;
                const std::uint64_t blendMs = static_cast<std::uint64_t>(o.blendS) * 1000u;
                if (blendMs == 0 || since >= blendMs)
                {
                    out.weatherFrom = o.weather;
                    out.weatherTo = o.weather;
                    out.weatherMix = 0.0f;
                }
                else
                {
                    // From the recorded weather that shows there (the pair's
                    // dominant type) into the override.
                    out.weatherFrom = recorded.weatherMix >= 0.5f ? recorded.weatherTo : recorded.weatherFrom;
                    out.weatherTo = o.weather;
                    out.weatherMix = std::clamp(static_cast<float>(since) / static_cast<float>(blendMs), 0.0f, 1.0f);
                }
            }
            return out;
        }

        // ---- Bytes (clip section ENVO and the project's clip entry), little
        // endian: i32 clock minutes, u32 weather, u16 blend s, u8 clouds,
        // u8 reserved (kBytesV1); then 12 bytes that held the experimental
        // sun and moon rows (i16 sun azimuth, i16 sun elevation, i8 sun fine,
        // i8 moon phase, i16 moon azimuth, i16 moon elevation, u16 reserved).
        // They are written as "Recorded" (an older build reads them as no
        // override) and ignored when read.
        constexpr std::size_t kBytesV1 = 12;
        constexpr std::size_t kBytes = 24;
        constexpr std::int16_t kLegacyRecordedAngle = -32768;

        inline void Pack(const EnvironmentOverride& o, std::uint8_t (&out)[kBytes])
        {
            const std::uint32_t clock = static_cast<std::uint32_t>(o.clockMinutes);
            for (int n = 0; n < 4; ++n)
            {
                out[n] = static_cast<std::uint8_t>(clock >> (8 * n));
                out[4 + n] = static_cast<std::uint8_t>(o.weather >> (8 * n));
            }
            out[8] = static_cast<std::uint8_t>(o.blendS);
            out[9] = static_cast<std::uint8_t>(o.blendS >> 8);
            out[10] = o.cloudsLive;
            out[11] = 0;
            const auto put16 = [&](std::size_t at, std::int16_t v) {
                out[at] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(v));
                out[at + 1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(v) >> 8);
            };
            put16(12, kLegacyRecordedAngle); // sun azimuth
            put16(14, kLegacyRecordedAngle); // sun elevation
            out[16] = 0;                     // sun fine
            out[17] = 0xFF;                  // moon phase -1
            put16(18, kLegacyRecordedAngle); // moon azimuth
            put16(20, kLegacyRecordedAngle); // moon elevation
            out[22] = 0;
            out[23] = 0;
        }

        // `size` kBytesV1 or kBytes (the legacy sun and moon bytes skipped,
        // whatever they hold). False (and `o` untouched) when the bytes are not a
        // valid value.
        inline bool Unpack(const std::uint8_t* in, std::size_t size, EnvironmentOverride& o)
        {
            if (size != kBytesV1 && size != kBytes)
                return false;
            EnvironmentOverride v;
            std::uint32_t clock = 0;
            std::uint32_t weather = 0;
            for (int n = 0; n < 4; ++n)
            {
                clock |= static_cast<std::uint32_t>(in[n]) << (8 * n);
                weather |= static_cast<std::uint32_t>(in[4 + n]) << (8 * n);
            }
            v.clockMinutes = static_cast<std::int32_t>(clock);
            v.weather = weather;
            v.blendS = static_cast<std::uint16_t>(in[8] | (in[9] << 8));
            v.cloudsLive = in[10];
            if (!Valid(v))
                return false;
            o = v;
            return true;
        }
    }
}
