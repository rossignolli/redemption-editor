#include "profiler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace replay::prof
{
    namespace
    {
        struct ZoneCounters
        {
            std::atomic<std::uint64_t> calls{0};
            std::atomic<std::uint64_t> ticks{0};
            std::atomic<std::uint64_t> maxTicks{0};
        };

        // A zone's counters in kZoneStripes stripes of one cache line, the stripe
        // picked by the calling thread. The detour zones are entered from every engine
        // job thread; with one set of counters every call would do 2 QPC and 5 locked
        // writes on lines every thread shares. Collect, EndFrame, ResetSession and the
        // reports sum calls and ticks over the stripes and take the largest max; a
        // reset exchanges every stripe.
        struct alignas(64) ZoneStripe
        {
            ZoneCounters window;
            ZoneCounters session;
            // This tick's ticks (script thread and detours).
            std::atomic<std::uint64_t> tickTicks{0};
            std::uint64_t padding = 0; // fills the line: the alignment adds no padding
        };
        static_assert(sizeof(ZoneStripe) == 64, "one cache line per stripe");

        struct Zone
        {
            ZoneStripe stripes[kZoneStripes];
        };

        Zone g_zones[kMaxZones];
        // The names apart (a pointer next to the stripes would pad every zone to
        // another line).
        const char* g_zoneNames[kMaxZones] = {};
        std::uint64_t g_slowFrameTicks = 0; // 0: from the default below
        double g_slowFrameUs = 50000.0;
        std::vector<SlowFrame> g_slowFrames;
        std::atomic<std::size_t> g_zoneCount{0};
        std::mutex g_registerLock;

        constexpr std::size_t kFrameRing = 2048;
        std::uint64_t g_frameRing[kFrameRing];
        std::size_t g_frameRingCount = 0; // frames in the current window (capped)
        std::uint64_t g_windowFrames = 0;
        std::uint64_t g_windowFrameTicks = 0;
        std::uint64_t g_windowFrameMax = 0;
        std::uint64_t g_sessionFrames = 0;
        std::uint64_t g_sessionFrameTicks = 0;
        std::uint64_t g_sessionFrameMax = 0;
        std::vector<double> g_sessionFrameSamples; // capped
        constexpr std::size_t kSessionSampleCap = 65536;

        std::uint64_t g_ticksPerSecond = 0;
        std::string g_lastWindowLine;

        // Game frame intervals by mode, as a histogram of 20 us bins up to the stall
        // limit (script thread only).
        constexpr double kGameFrameBinUs = 20.0;
        constexpr std::size_t kGameFrameBins = static_cast<std::size_t>(kGameFrameStallUs / kGameFrameBinUs);
        constexpr std::size_t kGameFrameModes = 6;
        struct GameFrameAccumulator
        {
            const char* mode = nullptr;
            std::uint64_t frames = 0;
            std::uint64_t stalls = 0;
            double totalUs = 0.0;
            double maxUs = 0.0;
            std::vector<std::uint32_t> bins;
        };
        struct GameFrameSet
        {
            GameFrameAccumulator modes[kGameFrameModes];
        };
        GameFrameSet g_gameFrameWindow;
        GameFrameSet g_gameFrameTotals;
        std::uint64_t g_lastGameFrameTick = 0;

        void AddGameFrame(GameFrameSet& set, const char* mode, double us)
        {
            GameFrameAccumulator* slot = nullptr;
            for (GameFrameAccumulator& candidate : set.modes)
            {
                if (candidate.mode && (candidate.mode == mode || std::strcmp(candidate.mode, mode) == 0))
                {
                    slot = &candidate;
                    break;
                }
                if (!candidate.mode && !slot)
                    slot = &candidate;
            }
            if (!slot)
                return; // more modes than slots: not measured
            if (!slot->mode)
                slot->mode = mode;
            if (!(us < kGameFrameStallUs))
            {
                ++slot->stalls;
                return;
            }
            if (slot->bins.empty())
                slot->bins.assign(kGameFrameBins, 0u);
            ++slot->frames;
            slot->totalUs += us;
            slot->maxUs = std::max(slot->maxUs, us);
            std::size_t bin = static_cast<std::size_t>(us / kGameFrameBinUs);
            if (bin >= kGameFrameBins)
                bin = kGameFrameBins - 1;
            ++slot->bins[bin];
        }

        double BinPercentile(const GameFrameAccumulator& slot, double fraction)
        {
            // Nearest rank, reported at the bin's upper edge.
            const double rank = std::ceil(fraction * static_cast<double>(slot.frames));
            std::uint64_t seen = 0;
            for (std::size_t bin = 0; bin < slot.bins.size(); ++bin)
            {
                seen += slot.bins[bin];
                if (static_cast<double>(seen) >= rank && seen > 0)
                    return std::min(static_cast<double>(bin + 1) * kGameFrameBinUs, slot.maxUs);
            }
            return slot.maxUs;
        }

        std::vector<GameFrameStats> TakeGameFrames(GameFrameSet& set)
        {
            std::vector<GameFrameStats> out;
            for (GameFrameAccumulator& slot : set.modes)
            {
                if (!slot.mode || (slot.frames == 0 && slot.stalls == 0))
                    continue;
                GameFrameStats stats;
                stats.mode = slot.mode;
                stats.frames = slot.frames;
                stats.stalls = slot.stalls;
                if (slot.frames)
                {
                    stats.avgUs = slot.totalUs / static_cast<double>(slot.frames);
                    stats.p50Us = BinPercentile(slot, 0.50);
                    stats.p95Us = BinPercentile(slot, 0.95);
                    stats.p99Us = BinPercentile(slot, 0.99);
                    stats.maxUs = slot.maxUs;
                    stats.fps = stats.avgUs > 0.0 ? 1000000.0 / stats.avgUs : 0.0;
                }
                out.push_back(stats);
            }
            set = GameFrameSet{};
            return out;
        }

        void WriteGameFrames(std::ostream& out, const char* tag, const std::vector<GameFrameStats>& frames)
        {
            char line[320]{};
            for (const GameFrameStats& stats : frames)
            {
                std::snprintf(line, sizeof(line),
                    "%s,mode=%s,frames=%llu,avg_us=%.1f,p50_us=%.0f,p95_us=%.0f,p99_us=%.0f,max_us=%.1f,fps=%.1f,"
                    "stalls=%llu\n",
                    tag, stats.mode ? stats.mode : "?", static_cast<unsigned long long>(stats.frames), stats.avgUs,
                    stats.p50Us, stats.p95Us, stats.p99Us, stats.maxUs, stats.fps,
                    static_cast<unsigned long long>(stats.stalls));
                out << line;
            }
        }

        std::uint64_t Frequency()
        {
            if (g_ticksPerSecond)
                return g_ticksPerSecond;
#ifdef _WIN32
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            g_ticksPerSecond = static_cast<std::uint64_t>(frequency.QuadPart);
#else
            g_ticksPerSecond = 1000000ull;
#endif
            return g_ticksPerSecond ? g_ticksPerSecond : 1ull;
        }

        double ToUs(std::uint64_t ticks)
        {
            return static_cast<double>(ticks) * 1000000.0 / static_cast<double>(Frequency());
        }

        void AddTo(ZoneCounters& counters, std::uint64_t ticks)
        {
            counters.calls.fetch_add(1, std::memory_order_relaxed);
            counters.ticks.fetch_add(ticks, std::memory_order_relaxed);
            std::uint64_t seen = counters.maxTicks.load(std::memory_order_relaxed);
            while (ticks > seen &&
                !counters.maxTicks.compare_exchange_weak(seen, ticks, std::memory_order_relaxed))
            {
            }
        }

        // The calling thread's stripe (thread ids are multiples of 4).
        std::size_t ThreadZoneStripe()
        {
#ifdef _WIN32
            return (GetCurrentThreadId() >> 2) & (kZoneStripes - 1);
#else
            return 0;
#endif
        }

        void AddToStripe(ZoneId zone, std::size_t stripe, std::uint64_t ticks)
        {
            ZoneStripe& counters = g_zones[zone].stripes[stripe];
            AddTo(counters.window, ticks);
            AddTo(counters.session, ticks);
            counters.tickTicks.fetch_add(ticks, std::memory_order_relaxed);
        }

        void Collect(std::vector<ZoneStats>& out, bool window, double frameTotalUs, bool reset)
        {
            out.clear();
            const std::size_t count = g_zoneCount.load(std::memory_order_acquire);
            for (std::size_t index = 0; index < count; ++index)
            {
                // The stripes' sums and largest max.
                std::uint64_t calls = 0;
                std::uint64_t ticks = 0;
                std::uint64_t maxTicks = 0;
                for (ZoneStripe& stripe : g_zones[index].stripes)
                {
                    ZoneCounters& counters = window ? stripe.window : stripe.session;
                    calls += reset ? counters.calls.exchange(0, std::memory_order_relaxed)
                                   : counters.calls.load(std::memory_order_relaxed);
                    ticks += reset ? counters.ticks.exchange(0, std::memory_order_relaxed)
                                   : counters.ticks.load(std::memory_order_relaxed);
                    maxTicks = std::max(maxTicks, reset ? counters.maxTicks.exchange(0, std::memory_order_relaxed)
                                                        : counters.maxTicks.load(std::memory_order_relaxed));
                }
                if (!calls)
                    continue;
                ZoneStats stats;
                stats.name = g_zoneNames[index];
                stats.calls = calls;
                stats.totalUs = ToUs(ticks);
                stats.avgUs = stats.totalUs / static_cast<double>(calls);
                stats.maxUs = ToUs(maxTicks);
                stats.framePct = frameTotalUs > 0.0 ? stats.totalUs * 100.0 / frameTotalUs : 0.0;
                out.push_back(stats);
            }
            std::sort(out.begin(), out.end(),
                [](const ZoneStats& a, const ZoneStats& b) { return a.totalUs > b.totalUs; });
        }

        FrameStats FrameFrom(std::uint64_t frames, std::uint64_t ticks, std::uint64_t maxTicks,
            std::vector<double>& samplesUs)
        {
            FrameStats stats;
            stats.frames = frames;
            stats.totalUs = ToUs(ticks);
            stats.avgUs = frames ? stats.totalUs / static_cast<double>(frames) : 0.0;
            stats.maxUs = ToUs(maxTicks);
            std::sort(samplesUs.begin(), samplesUs.end());
            stats.p50Us = Percentile(samplesUs, 0.50);
            stats.p95Us = Percentile(samplesUs, 0.95);
            return stats;
        }

        void WriteZones(std::ostream& out, const char* tag, const char* mode, const Snapshot& snapshot)
        {
            char line[320]{};
            std::snprintf(line, sizeof(line),
                "%s_FRAME,mode=%s,frames=%llu,avg_us=%.1f,p50_us=%.1f,p95_us=%.1f,max_us=%.1f,total_ms=%.1f\n",
                tag, mode, static_cast<unsigned long long>(snapshot.frame.frames), snapshot.frame.avgUs,
                snapshot.frame.p50Us, snapshot.frame.p95Us, snapshot.frame.maxUs,
                snapshot.frame.totalUs / 1000.0);
            out << line;
            for (const ZoneStats& zone : snapshot.zones)
            {
                std::snprintf(line, sizeof(line),
                    "%s_ZONE,mode=%s,zone=%s,calls=%llu,avg_us=%.1f,max_us=%.1f,total_ms=%.2f,frame_pct=%.1f\n",
                    tag, mode, zone.name ? zone.name : "?", static_cast<unsigned long long>(zone.calls),
                    zone.avgUs, zone.maxUs, zone.totalUs / 1000.0, zone.framePct);
                out << line;
            }
        }
    }

    ZoneId Register(const char* name)
    {
        if (!name)
            return kInvalidZone;
        std::lock_guard<std::mutex> lock(g_registerLock);
        const std::size_t count = g_zoneCount.load(std::memory_order_acquire);
        for (std::size_t index = 0; index < count; ++index)
        {
            if (g_zoneNames[index] == name || std::strcmp(g_zoneNames[index], name) == 0)
                return static_cast<ZoneId>(index);
        }
        if (count >= kMaxZones)
            return kInvalidZone;
        g_zoneNames[count] = name;
        g_zoneCount.store(count + 1, std::memory_order_release);
        return static_cast<ZoneId>(count);
    }

    const char* Name(ZoneId zone)
    {
        return zone < g_zoneCount.load(std::memory_order_acquire) ? g_zoneNames[zone] : nullptr;
    }

    std::uint64_t Now()
    {
#ifdef _WIN32
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<std::uint64_t>(now.QuadPart);
#else
        return 0;
#endif
    }

    std::uint64_t TicksPerSecond() { return Frequency(); }

    void SetTicksPerSecondForTests(std::uint64_t ticksPerSecond)
    {
        g_ticksPerSecond = ticksPerSecond;
        g_slowFrameTicks = 0;
    }

    void Add(ZoneId zone, std::uint64_t ticks)
    {
        if (zone >= g_zoneCount.load(std::memory_order_acquire))
            return;
        AddToStripe(zone, ThreadZoneStripe(), ticks);
    }

    void AddToStripeForTests(ZoneId zone, std::size_t stripe, std::uint64_t ticks)
    {
        if (zone >= g_zoneCount.load(std::memory_order_acquire) || stripe >= kZoneStripes)
            return;
        AddToStripe(zone, stripe, ticks);
    }

    void SetSlowFrameUs(double us)
    {
        g_slowFrameUs = us > 0.0 ? us : 50000.0;
        g_slowFrameTicks = 0;
    }

    void EndFrame(std::uint64_t ticks)
    {
        {
            // The zone breakdown of a slow tick, then this tick's per-zone time starts over.
            if (!g_slowFrameTicks)
                g_slowFrameTicks = static_cast<std::uint64_t>(g_slowFrameUs * static_cast<double>(Frequency()) / 1000000.0);
            const std::size_t count = g_zoneCount.load(std::memory_order_acquire);
            const bool slow = ticks >= g_slowFrameTicks && g_slowFrames.size() < kMaxSlowFrames;
            SlowFrame frame;
            if (slow)
                frame.frameUs = ToUs(ticks);
            for (std::size_t index = 0; index < count; ++index)
            {
                std::uint64_t zoneTicks = 0; // every stripe's, each reset
                for (ZoneStripe& stripe : g_zones[index].stripes)
                    zoneTicks += stripe.tickTicks.exchange(0, std::memory_order_relaxed);
                if (!slow || !zoneTicks)
                    continue;
                // Insert by size into the top kSlowFrameZones.
                const double us = ToUs(zoneTicks);
                std::size_t at = frame.zoneCount;
                while (at > 0 && frame.zoneUs[at - 1] < us)
                    --at;
                if (at >= kSlowFrameZones)
                    continue;
                const std::size_t last = frame.zoneCount < kSlowFrameZones ? frame.zoneCount : kSlowFrameZones - 1;
                for (std::size_t move = last; move > at; --move)
                {
                    frame.zones[move] = frame.zones[move - 1];
                    frame.zoneUs[move] = frame.zoneUs[move - 1];
                }
                frame.zones[at] = g_zoneNames[index];
                frame.zoneUs[at] = us;
                if (frame.zoneCount < kSlowFrameZones)
                    ++frame.zoneCount;
            }
            if (slow)
                g_slowFrames.push_back(frame);
        }
        if (g_frameRingCount < kFrameRing)
            g_frameRing[g_frameRingCount++] = ticks;
        ++g_windowFrames;
        g_windowFrameTicks += ticks;
        g_windowFrameMax = std::max(g_windowFrameMax, ticks);
        ++g_sessionFrames;
        g_sessionFrameTicks += ticks;
        g_sessionFrameMax = std::max(g_sessionFrameMax, ticks);
        if (g_sessionFrameSamples.size() < kSessionSampleCap)
            g_sessionFrameSamples.push_back(ToUs(ticks));
    }

    void MarkGameFrame(std::uint64_t now, const char* mode)
    {
        if (!mode)
            mode = "?";
        if (g_lastGameFrameTick && now > g_lastGameFrameTick)
        {
            const double us = ToUs(now - g_lastGameFrameTick);
            AddGameFrame(g_gameFrameWindow, mode, us);
            AddGameFrame(g_gameFrameTotals, mode, us);
        }
        g_lastGameFrameTick = now;
    }

    std::vector<GameFrameStats> TakeGameFrameTotals() { return TakeGameFrames(g_gameFrameTotals); }

    void WriteGameFrameTotals(std::ostream& out)
    {
        WriteGameFrames(out, "PROFILE_TOTAL_GAME_FRAME", TakeGameFrameTotals());
    }

    Snapshot TakeWindow()
    {
        Snapshot snapshot;
        std::vector<double> samples;
        samples.reserve(g_frameRingCount);
        for (std::size_t index = 0; index < g_frameRingCount; ++index)
            samples.push_back(ToUs(g_frameRing[index]));
        snapshot.frame = FrameFrom(g_windowFrames, g_windowFrameTicks, g_windowFrameMax, samples);
        Collect(snapshot.zones, true, snapshot.frame.totalUs, true);
        snapshot.slow.swap(g_slowFrames);
        g_slowFrames.clear();
        snapshot.gameFrames = TakeGameFrames(g_gameFrameWindow);
        g_frameRingCount = 0;
        g_windowFrames = 0;
        g_windowFrameTicks = 0;
        g_windowFrameMax = 0;
        return snapshot;
    }

    Snapshot Session()
    {
        Snapshot snapshot;
        std::vector<double> samples = g_sessionFrameSamples;
        snapshot.frame = FrameFrom(g_sessionFrames, g_sessionFrameTicks, g_sessionFrameMax, samples);
        Collect(snapshot.zones, false, snapshot.frame.totalUs, false);
        return snapshot;
    }

    std::uint64_t SessionFrameCount() { return g_sessionFrames; }

    void ResetSession()
    {
        const std::size_t count = g_zoneCount.load(std::memory_order_acquire);
        for (std::size_t index = 0; index < count; ++index)
        {
            for (ZoneStripe& stripe : g_zones[index].stripes) // every stripe
            {
                for (ZoneCounters* counters : {&stripe.window, &stripe.session})
                {
                    counters->calls.store(0, std::memory_order_relaxed);
                    counters->ticks.store(0, std::memory_order_relaxed);
                    counters->maxTicks.store(0, std::memory_order_relaxed);
                }
            }
        }
        g_frameRingCount = 0;
        g_windowFrames = 0;
        g_windowFrameTicks = 0;
        g_windowFrameMax = 0;
        g_sessionFrames = 0;
        g_sessionFrameTicks = 0;
        g_sessionFrameMax = 0;
        g_sessionFrameSamples.clear();
        g_lastWindowLine.clear();
        g_slowFrames.clear();
        for (std::size_t index = 0; index < count; ++index)
            for (ZoneStripe& stripe : g_zones[index].stripes)
                stripe.tickTicks.store(0, std::memory_order_relaxed);
    }

    void WriteWindowReport(std::ostream& out, const char* mode, std::uint32_t windowMs)
    {
        const Snapshot snapshot = TakeWindow();
        char header[160]{};
        std::snprintf(header, sizeof(header), "PROFILE_WINDOW,mode=%s,window_ms=%u\n", mode, windowMs);
        out << header;
        WriteZones(out, "PROFILE", mode, snapshot);
        WriteGameFrames(out, "PROFILE_GAME_FRAME", snapshot.gameFrames);
        // One line per slow tick of the window, its largest zones.
        for (const SlowFrame& slow : snapshot.slow)
        {
            char text[640]{};
            int used = std::snprintf(text, sizeof(text), "PROFILE_SLOW_TICK,mode=%s,tick_ms=%.1f,zones=", mode,
                slow.frameUs / 1000.0);
            for (std::size_t index = 0; index < slow.zoneCount && used > 0 && static_cast<std::size_t>(used) < sizeof(text); ++index)
                used += std::snprintf(text + used, sizeof(text) - static_cast<std::size_t>(used), "%s%s:%.1f",
                    index ? ";" : "", slow.zones[index] ? slow.zones[index] : "?", slow.zoneUs[index] / 1000.0);
            out << text << '\n';
        }
        char line[200]{};
        int written = std::snprintf(line, sizeof(line), "tick p95 %.2f ms", snapshot.frame.p95Us / 1000.0);
        for (std::size_t index = 0; index < snapshot.zones.size() && index < 3; ++index)
        {
            if (written < 0 || static_cast<std::size_t>(written) >= sizeof(line))
                break;
            written += std::snprintf(line + written, sizeof(line) - static_cast<std::size_t>(written),
                " | %s %.2f", snapshot.zones[index].name, snapshot.zones[index].totalUs / 1000.0 /
                    static_cast<double>(snapshot.frame.frames ? snapshot.frame.frames : 1));
        }
        g_lastWindowLine = line;
    }

    void WriteSessionReport(std::ostream& out, const char* mode)
    {
        WriteZones(out, "PROFILE_TOTAL", mode, Session());
        WriteGameFrameTotals(out);
    }

    std::string LastWindowLine() { return g_lastWindowLine; }

    double Percentile(const std::vector<double>& sortedUs, double fraction)
    {
        if (sortedUs.empty())
            return 0.0;
        const double rank = std::ceil(fraction * static_cast<double>(sortedUs.size()));
        std::size_t index = rank <= 1.0 ? 0 : static_cast<std::size_t>(rank) - 1;
        if (index >= sortedUs.size())
            index = sortedUs.size() - 1;
        return sortedUs[index];
    }
}
