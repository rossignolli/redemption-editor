#pragma once

// The one microsecond clock of the mod.
//
// `counter * 1000000 / frequency` in 64 bits overflows once the counter
// passes 2^64 / 10^6: after ~21 days of uptime with the usual 10 MHz
// QueryPerformanceCounter, every event time would jump back to near zero.
// TicksToUs splits the division so it never overflows for any counter
// value.
//
// TicksToUs is pure (unit tested); NowUs reads QueryPerformanceCounter and
// may be called from any thread (engine detours included).

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

namespace replay::clock
{
    inline constexpr std::uint64_t TicksToUs(std::uint64_t ticks, std::uint64_t perSecond)
    {
        return perSecond == 0 ? 0 : ticks / perSecond * 1000000ull + ticks % perSecond * 1000000ull / perSecond;
    }

#ifdef _WIN32
    inline std::uint64_t Frequency()
    {
        // The frequency is fixed at boot; a racing first call stores the
        // same value twice.
        static const std::uint64_t frequency = [] {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return static_cast<std::uint64_t>(value.QuadPart);
        }();
        return frequency;
    }

    inline std::uint64_t NowUs()
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return TicksToUs(static_cast<std::uint64_t>(now.QuadPart), Frequency());
    }
#endif
}
