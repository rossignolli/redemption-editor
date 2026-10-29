// src/env_override.h, the editor's environment settings of a clip. Recorded
// is every setting's first value and gives back exactly the recorded state;
// an override clock keeps the clip's own pace from the chosen start; the
// weather blend is a function of the playhead alone (scrub, jump and export
// give the same state); the steps cycle through Recorded; the bytes round
// trip and a bad value is refused.
#include "env_override.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace
{
    using replay::EnvironmentOverride;
    namespace env = replay::envoverride;

    int g_envFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_envFailures;
    }

    bool Near(float a, float b) { return std::fabs(a - b) <= 1e-4f; }

    void TestHashes()
    {
        // GET_HASH_KEY values the game's scripts compare with (joaat).
        Check(env::Joaat("SUNNY") == 0x614A1F91u, "joaat SUNNY");
        Check(env::Joaat("CLOUDS") == 0x30FDAF5Cu, "joaat CLOUDS");
        Check(env::Joaat("HIGHPRESSURE") == 0xF5A87B65u, "joaat HIGHPRESSURE");
        Check(env::Joaat("rain") == env::Joaat("RAIN") && env::Joaat("RAIN") == 0x54A69840u, "joaat ignores case");
        Check(env::WeatherIndex(0x30FDAF5Cu) == 2 && env::WeatherIndex(0x12345678u) == -1, "weather index of a hash");
        for (std::uint32_t n = 0; n < env::kWeatherCount; ++n)
            for (std::uint32_t m = n + 1; m < env::kWeatherCount; ++m)
                Check(env::WeatherHash(n) != env::WeatherHash(m), "weather hashes are distinct");
    }

    void TestRecordedIsUntouched()
    {
        env::Recorded recorded;
        recorded.clockS = 50000;
        recorded.startClockS = 49000;
        recorded.weatherFrom = env::Joaat("CLOUDS");
        recorded.weatherTo = env::Joaat("RAIN");
        recorded.weatherMix = 0.3f;
        const env::Applied a = env::Resolve(recorded, EnvironmentOverride{}, 12345, 0);
        Check(a.clockS == 50000 && a.weatherFrom == recorded.weatherFrom && a.weatherTo == recorded.weatherTo &&
                Near(a.weatherMix, 0.3f) && a.pinClouds && !a.clockOverridden && !a.weatherOverridden,
            "all Recorded: exactly the recorded state");
        Check(!EnvironmentOverride{}.Any(), "the default is all Recorded");
    }

    void TestClockOverride()
    {
        EnvironmentOverride o;
        o.clockMinutes = 6 * 60; // 06:00 at the clip's start
        env::Recorded recorded;
        recorded.startClockS = 14 * 3600;          // recorded from 14:00
        recorded.clockS = 14 * 3600 + 90;          // 90 s of game clock later
        const env::Applied a = env::Resolve(recorded, o, 45000, 0);
        Check(a.clockOverridden && a.clockS == 6 * 3600 + 90, "the clock keeps the clip's pace from the chosen start");
        // Across midnight on both sides.
        recorded.startClockS = 23 * 3600 + 59 * 60;
        recorded.clockS = 120; // 00:02, 3 minutes later
        o.clockMinutes = 23 * 60 + 45;
        const env::Applied b = env::Resolve(recorded, o, 0, 0);
        Check(b.clockS == 23 * 3600 + 48 * 60, "recorded flow across midnight");
        o.clockMinutes = 23 * 60 + 59;
        recorded.startClockS = 0;
        recorded.clockS = 120;
        const env::Applied c = env::Resolve(recorded, o, 0, 0);
        Check(c.clockS == 60, "override clock wraps past midnight");
    }

    void TestWeatherOverrideAndBlend()
    {
        env::Recorded recorded;
        recorded.weatherFrom = env::Joaat("SUNNY");
        recorded.weatherTo = env::Joaat("CLOUDS");
        recorded.weatherMix = 0.8f; // shows mostly CLOUDS
        EnvironmentOverride o;
        o.weather = env::Joaat("RAIN");
        const env::Applied cut = env::Resolve(recorded, o, 0, 0);
        Check(cut.weatherOverridden && cut.weatherFrom == o.weather && cut.weatherTo == o.weather && Near(cut.weatherMix, 0.0f),
            "a cut: the override from the first frame");
        o.blendS = 10;
        const std::uint64_t origin = 3000; // a project clip trimmed at 3 s
        const env::Applied before = env::Resolve(recorded, o, 1000, origin);
        Check(before.weatherFrom == recorded.weatherTo && before.weatherTo == o.weather && Near(before.weatherMix, 0.0f),
            "before the origin: the recorded weather that shows");
        const env::Applied half = env::Resolve(recorded, o, origin + 5000, origin);
        Check(half.weatherFrom == recorded.weatherTo && half.weatherTo == o.weather && Near(half.weatherMix, 0.5f),
            "half way through the blend");
        const env::Applied done = env::Resolve(recorded, o, origin + 10000, origin);
        Check(done.weatherFrom == o.weather && done.weatherTo == o.weather, "the blend ends on the override");
        // The same playhead, the same state, whatever came before (scrub).
        const env::Applied again = env::Resolve(recorded, o, origin + 5000, origin);
        Check(again.weatherFrom == half.weatherFrom && Near(again.weatherMix, half.weatherMix), "same playhead, same blend");
        o.cloudsLive = 1;
        Check(!env::Resolve(recorded, o, 0, 0).pinClouds, "clouds live: the track is not pinned");
    }

    void TestSteps()
    {
        // Clock: out of Recorded near the recorded start, 15 minute steps,
        // back to Recorded past either end; left of Recorded is Live.
        const std::uint32_t start = 14 * 3600 + 7 * 60 + 30; // 14:07:30
        Check(env::StepClock(-1, start, 1) == 14 * 60, "first step right: the recorded start rounded down");
        Check(env::StepClock(-1, start, -1) == env::kClockLive, "first step left: Live");
        Check(env::StepClock(env::kClockLive, start, 1) == -1 && env::StepClock(env::kClockLive, start, -1) == env::kClockLive,
            "Live is the row's end: right goes back to Recorded");
        Check(env::StepClock(14 * 60, start, 1) == 14 * 60 + 15 && env::StepClock(14 * 60, start, -1) == 13 * 60 + 45, "15 minute steps");
        Check(env::StepClock(14 * 60 + 7, start, -1) == 14 * 60 && env::StepClock(14 * 60 + 7, start, 1) == 14 * 60 + 15,
            "an odd minute steps onto the grid");
        Check(env::StepClock(23 * 60 + 45, start, 1) == -1 && env::StepClock(0, start, -1) == -1, "past the ends: Recorded");
        Check(env::StepClock(600, start, 0) == 600, "no step, no change");
        // Weather: a ring Recorded, list..., Live, Recorded.
        std::uint32_t w = 0;
        w = env::StepWeather(w, 1);
        Check(w == env::WeatherHash(0), "right from Recorded: the first type");
        Check(env::StepWeather(0, -1) == env::kWeatherLive, "left from Recorded: Live");
        Check(env::StepWeather(env::kWeatherLive, -1) == env::WeatherHash(env::kWeatherCount - 1), "left from Live: the last type");
        Check(env::StepWeather(env::WeatherHash(env::kWeatherCount - 1), 1) == env::kWeatherLive, "right from the last: Live");
        Check(env::StepWeather(env::kWeatherLive, 1) == 0, "right from Live: Recorded");
        Check(env::StepWeather(0x12345678u, 1) == env::WeatherHash(0), "an unknown hash steps from Recorded");
        std::uint32_t ring = 0;
        for (std::uint32_t n = 0; n <= env::kWeatherCount + 1; ++n)
            ring = env::StepWeather(ring, 1);
        Check(ring == 0, "the weather ring comes back to Recorded");
        // Blend.
        Check(env::StepBlend(0, 1) == 2 && env::StepBlend(0, -1) == 30 && env::StepBlend(30, 1) == 0, "blend ring");
    }

    void TestBytes()
    {
        EnvironmentOverride o;
        o.clockMinutes = 19 * 60 + 30;
        o.weather = env::Joaat("FOG");
        o.blendS = 5;
        o.cloudsLive = 1;
        std::uint8_t bytes[env::kBytes];
        env::Pack(o, bytes);
        EnvironmentOverride back;
        Check(env::Unpack(bytes, env::kBytes, back) && back == o, "bytes round trip");
        EnvironmentOverride recorded;
        env::Pack(recorded, bytes);
        Check(env::Unpack(bytes, env::kBytes, back) && back == recorded && back.clockMinutes == -1, "Recorded round trips (-1 clock)");
        // `value` is XORed into the byte (it always changes it).
        const auto refused = [&](std::size_t at, std::uint8_t value, const char* what) {
            std::uint8_t bad[env::kBytes];
            env::Pack(o, bad);
            bad[at] = static_cast<std::uint8_t>(bad[at] ^ value);
            EnvironmentOverride kept = o;
            kept.blendS = 2;
            Check(!env::Unpack(bad, env::kBytes, kept) && kept.blendS == 2, what);
        };
        refused(4, 0x01, "an unknown weather hash is refused");
        refused(8, 0x02, "a blend not in the list is refused"); // 5 -> 7
        refused(10, 0x02, "clouds other than 0 or 1 refused"); // 1 -> 3
        std::uint8_t late[env::kBytes];
        EnvironmentOverride day = o;
        day.clockMinutes = 24 * 60;
        env::Pack(day, late);
        Check(!env::Unpack(late, env::kBytes, back), "a clock past the day is refused");
    }

    // Live leaves the clock and the weather to the game; the recorded fields
    // stay for the log.
    void TestLive()
    {
        env::Recorded recorded;
        recorded.clockS = 50000;
        recorded.startClockS = 49000;
        recorded.weatherFrom = env::Joaat("SNOWLIGHT");
        recorded.weatherTo = env::Joaat("GROUNDBLIZZARD");
        recorded.weatherMix = 0.4f;
        EnvironmentOverride o;
        o.clockMinutes = env::kClockLive;
        o.weather = env::kWeatherLive;
        o.blendS = 10;
        const env::Applied a = env::Resolve(recorded, o, 5000, 0);
        Check(a.clockLive && a.weatherLive && !a.clockOverridden && !a.weatherOverridden, "Live: nothing for the replay to set");
        Check(a.clockS == 50000 && a.weatherFrom == recorded.weatherFrom && a.weatherTo == recorded.weatherTo &&
                Near(a.weatherMix, 0.4f),
            "Live keeps the recorded state in the fields (the log compares)");
        Check(o.Any() && o.ClockLive() && o.WeatherLive(), "Live is a setting");
        Check(!env::Resolve(recorded, EnvironmentOverride{}, 5000, 0).clockLive, "Recorded is not Live");
        std::uint8_t bytes[env::kBytes];
        env::Pack(o, bytes);
        EnvironmentOverride back;
        Check(env::Unpack(bytes, env::kBytes, back) && back == o, "Live round trips in the bytes");
        EnvironmentOverride below = o;
        below.clockMinutes = -3;
        env::Pack(below, bytes);
        Check(!env::Unpack(bytes, env::kBytes, back), "a clock under Live is refused");
    }

    // The legacy sun and moon bytes (ENVO v2, the experimental rows cut from
    // 1.0): written as Recorded, ignored when read, whatever they hold, so a
    // clip saved with them still opens and plays its recorded sky.
    void TestLegacySunAndMoonBytes()
    {
        EnvironmentOverride o;
        o.clockMinutes = 6 * 60;
        std::uint8_t bytes[env::kBytes];
        env::Pack(o, bytes);
        const auto get16 = [&](std::size_t at) {
            return static_cast<std::int16_t>(static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8)));
        };
        Check(get16(12) == -32768 && get16(14) == -32768 && bytes[16] == 0 && bytes[17] == 0xFF && get16(18) == -32768 &&
                get16(20) == -32768 && bytes[22] == 0 && bytes[23] == 0,
            "the legacy bytes are written as Recorded");
        // A value a dev build wrote with the sun and the moon set (and one
        // out of the old ranges): it loads, the rest of the settings kept.
        bytes[12] = 232 & 0xFF;
        bytes[13] = 0;
        bytes[14] = 91; // an elevation the old rule refused
        bytes[16] = static_cast<std::uint8_t>(-3);
        bytes[17] = 45;
        bytes[18] = 0x68; // azimuth 360
        bytes[19] = 0x01;
        EnvironmentOverride back;
        Check(env::Unpack(bytes, env::kBytes, back) && back == o && back.Any(), "legacy sun and moon bytes ignored");
        EnvironmentOverride old;
        Check(env::Unpack(bytes, env::kBytesV1, old) && old == o, "a 12 byte value reads the same settings");
        Check(!env::Unpack(bytes, 13, old), "another size refused");
    }
}

int RunEnvOverrideTests()
{
    g_envFailures = 0;
    TestHashes();
    TestRecordedIsUntouched();
    TestClockOverride();
    TestWeatherOverrideAndBlend();
    TestSteps();
    TestBytes();
    TestLive();
    TestLegacySunAndMoonBytes();
    if (g_envFailures == 0)
        std::printf("env override tests passed\n");
    return g_envFailures;
}
