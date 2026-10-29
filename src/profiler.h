#pragma once

// Lightweight zone profiler for the replay mod.
//
// Zones are registered once by name and accumulate calls, total time and
// maximum time with atomics, so a zone can be entered from the script thread
// or from an engine detour. The script tick closes a frame with EndFrame;
// frame durations feed a ring for percentiles. A report window is what has
// accumulated since the previous report: the log gets one PROFILE_FRAME line
// and one PROFILE_ZONE line per active zone, and PROFILE_TOTAL lines carry
// the session totals when a recording or a playback stops.
//
// Pure data apart from Now() (and the calling thread's counter stripe): the
// accumulation and the report are unit tested with synthetic ticks
// (ReplayCoreTests).

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace replay::prof
{
    using ZoneId = std::uint16_t;
    constexpr ZoneId kInvalidZone = 0xFFFF;
    constexpr std::size_t kMaxZones = 192; // a zone past the limit is silently not measured

    // Idempotent by name (pointer to a string literal or other static
    // storage). Returns kInvalidZone when the table is full.
    ZoneId Register(const char* name);
    const char* Name(ZoneId zone);

    // Ticks of the time source. Windows: QueryPerformanceCounter.
    std::uint64_t Now();
    std::uint64_t TicksPerSecond();
    // Tests inject a frequency so synthetic ticks convert predictably.
    void SetTicksPerSecondForTests(std::uint64_t ticksPerSecond);

    // Accumulate one call of `ticks` into a zone (thread-safe).
    void Add(ZoneId zone, std::uint64_t ticks);

    // Every zone keeps its counters in kZoneStripes stripes of one cache line each;
    // Add takes the calling thread's stripe, and every read sums the calls and
    // ticks of the stripes and takes the largest max. Tests: the same call added
    // to a chosen stripe (an index past the last is ignored).
    constexpr std::size_t kZoneStripes = 4;
    void AddToStripeForTests(ZoneId zone, std::size_t stripe, std::uint64_t ticks);

    struct Scope
    {
        explicit Scope(ZoneId zone) : m_zone(zone), m_start(Now()) {}
        ~Scope()
        {
            if (m_zone != kInvalidZone)
                Add(m_zone, Now() - m_start);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        ZoneId m_zone;
        std::uint64_t m_start;
    };

    // Script thread: one whole script tick took `ticks`.
    void EndFrame(std::uint64_t ticks);

    // The game frame, measured as the time between two script ticks (the script
    // runs once per game frame), by mode (idle, buffer, record, play; the mode of
    // the tick that ends the interval). PROFILE_TOTAL_FRAME only measures our
    // tick; the capture worker and the engine threads cost frame time without
    // showing there, so an A/B of the buffer on and off (or of two builds) reads
    // this. `now` in Now() ticks, once per tick. Intervals of kGameFrameStallUs or
    // more (pause menu, loading, a debugger) are counted as stalls and left out of
    // the statistics.
    constexpr double kGameFrameStallUs = 250000.0;
    void MarkGameFrame(std::uint64_t now, const char* mode);
    struct GameFrameStats
    {
        const char* mode = nullptr;
        std::uint64_t frames = 0;
        std::uint64_t stalls = 0;
        double avgUs = 0.0;
        double p50Us = 0.0;
        double p95Us = 0.0;
        double p99Us = 0.0;
        double maxUs = 0.0;
        double fps = 0.0; // 1e6 / avgUs
    };
    // Every mode with frames since the last take (the window: since the
    // last TakeWindow; the totals: since the last TakeGameFrameTotals, not
    // cleared by ResetSession, so the idle frames before a recording are
    // kept until the stop's report). Percentiles from 20 us bins.
    std::vector<GameFrameStats> TakeGameFrameTotals();
    // PROFILE_TOTAL_GAME_FRAME lines of TakeGameFrameTotals (written by
    // WriteSessionReport); the window report writes PROFILE_GAME_FRAME.
    void WriteGameFrameTotals(std::ostream& out);

    // A tick at or over this many microseconds keeps its own zone breakdown (the
    // window lines only give each zone's max, not which zones made up the slow
    // tick). Up to kMaxSlowFrames per window.
    constexpr std::size_t kMaxSlowFrames = 8;
    constexpr std::size_t kSlowFrameZones = 8;
    void SetSlowFrameUs(double us);
    struct SlowFrame
    {
        double frameUs = 0.0;
        std::size_t zoneCount = 0;
        const char* zones[kSlowFrameZones] = {};
        double zoneUs[kSlowFrameZones] = {}; // largest first; nested zones count in their parent too
    };

    struct ZoneStats
    {
        const char* name = nullptr;
        std::uint64_t calls = 0;
        double totalUs = 0.0;
        double avgUs = 0.0;
        double maxUs = 0.0;
        double framePct = 0.0; // share of the window's summed frame time
    };

    struct FrameStats
    {
        std::uint64_t frames = 0;
        double totalUs = 0.0;
        double avgUs = 0.0;
        double p50Us = 0.0;
        double p95Us = 0.0;
        double maxUs = 0.0;
    };

    struct Snapshot
    {
        FrameStats frame;
        std::vector<ZoneStats> zones; // active zones, largest total first
        std::vector<SlowFrame> slow;  // window only
        std::vector<GameFrameStats> gameFrames; // window only
    };

    // Window since the previous TakeWindow (resets the window counters).
    Snapshot TakeWindow();
    // Everything since ResetSession.
    Snapshot Session();
    // The ticks since ResetSession, without building a Snapshot.
    std::uint64_t SessionFrameCount();
    void ResetSession();

    // Log lines. `mode` names what ran (record, play, idle).
    void WriteWindowReport(std::ostream& out, const char* mode, std::uint32_t windowMs);
    void WriteSessionReport(std::ostream& out, const char* mode);
    // One overlay line: tick p95 and the three largest zones of the last
    // window report.
    std::string LastWindowLine();

    // Percentile of sorted samples (nearest rank). Exposed for tests.
    double Percentile(const std::vector<double>& sortedUs, double fraction);
}

// Zone scope for a function body: registered once per site.
#define REPLAY_PROFILE_ZONE(nameLiteral)                                                 \
    static const ::replay::prof::ZoneId replayProfileZoneId_ = ::replay::prof::Register(nameLiteral); \
    ::replay::prof::Scope replayProfileScope_(replayProfileZoneId_)
