#pragma once

// Pure rules of the replay buffer's real window and of the manual recording's
// part names, so the core tests cover them.
//
// The ring keeps ReplayBufferSeconds while its recording fits in
// ReplayBufferMB; past the budget the head moves on and the window shrinks
// (Saint Denis, 220 to 260 entities: 6.3 to 7.4 MB/s, 128 MB hold 17 to 20 s).
// The buffer says what it really covers (the reason it is short) and, with
// ReplayBufferAdaptive=1, grows its budget up to ReplayBufferMaxMB to keep the
// seconds, never while the machine is short of free memory.
//
// A manual recording is saved in parts (chained recordings): a part closes at
// ManualMaxMB, with the buffer on or off (with the buffer on, at its budget
// when that is larger), never past what a clip file may hold. Each part gets
// its own number, the final save included.

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace replay::buffercov
{
    // What limits the window the ring holds now.
    enum class Limit
    {
        Time,   // the whole ReplayBufferSeconds window
        Start,  // the buffer started less than the window ago
        Manual, // a running manual recording holds the head at its start
        Memory, // the budget moved the head on: the window is shorter
    };

    inline const char* LimitText(Limit limit)
    {
        switch (limit)
        {
        case Limit::Time: return "time";
        case Limit::Start: return "start";
        case Limit::Manual: return "manual";
        default: return "memory";
        }
    }

    struct Coverage
    {
        std::uint32_t coveredMs = 0;
        std::uint32_t targetMs = 0;
        Limit limit = Limit::Time;
    };

    // The ring head moves in blocks (2 s): a window within one block of the
    // target counts as the whole window.
    constexpr std::uint32_t kBlockMs = 2000;

    // nowMs and headMs on the recording clock; manualStartMs only when a
    // manual mark is running inside the ring.
    inline Coverage Evaluate(std::uint32_t nowMs, std::uint32_t headMs, std::uint32_t targetSeconds,
        bool manualInBuffer, std::uint32_t manualStartMs)
    {
        Coverage coverage;
        coverage.coveredMs = nowMs > headMs ? nowMs - headMs : 0;
        coverage.targetMs = targetSeconds * 1000u;
        if (manualInBuffer && headMs <= manualStartMs && coverage.coveredMs > coverage.targetMs)
            coverage.limit = Limit::Manual;
        else if (coverage.coveredMs + kBlockMs >= coverage.targetMs)
            coverage.limit = Limit::Time;
        else if (headMs == 0)
            coverage.limit = Limit::Start;
        else
            coverage.limit = Limit::Memory;
        return coverage;
    }

    // A save shorter than 90 % of the window because of the memory budget tells
    // the player.
    inline bool ShortWindowNotice(const Coverage& coverage)
    {
        return coverage.limit == Limit::Memory &&
            static_cast<std::uint64_t>(coverage.coveredMs) * 10u < static_cast<std::uint64_t>(coverage.targetMs) * 9u;
    }

    // ReplayBufferAdaptive: the budget the ring trims to. Off: the
    // configured ReplayBufferMB. On: up to maxMB while the machine has more
    // than reserveMB of physical memory free (the save's cut copies the
    // window once more), back to the configured budget below that.
    struct AdaptiveConfig
    {
        bool enabled = false;
        std::uint32_t baseMB = 128;
        std::uint32_t maxMB = 512;
        std::uint32_t reserveMB = 2048;
    };

    struct Budget
    {
        std::size_t bytes = 0;
        bool grown = false;   // above the configured budget
        bool memoryLow = false; // the free memory kept it at the configured one
    };

    inline Budget AdaptiveBudget(const AdaptiveConfig& config, std::size_t usedBytes, std::uint64_t availPhysMB)
    {
        Budget budget;
        const std::size_t base = static_cast<std::size_t>(config.baseMB) * 1024 * 1024;
        budget.bytes = base;
        if (!config.enabled || config.maxMB <= config.baseMB)
            return budget;
        // Each MB the ring grows past the base comes out of free memory:
        // it may grow while the rest stays above the reserve.
        const std::uint64_t usedMB = usedBytes / (1024 * 1024);
        const std::uint64_t grownMB = usedMB > config.baseMB ? usedMB - config.baseMB : 0;
        const std::uint64_t free = availPhysMB + grownMB; // what it had before growing
        if (free <= config.reserveMB)
        {
            budget.memoryLow = true;
            return budget;
        }
        const std::uint64_t room = free - config.reserveMB;
        const std::uint64_t capMB = (std::min<std::uint64_t>)(config.maxMB, config.baseMB + room);
        budget.bytes = static_cast<std::size_t>(capMB) * 1024 * 1024;
        budget.grown = capMB > config.baseMB;
        budget.memoryLow = capMB < config.maxMB;
        return budget;
    }

    // The budget that would hold `seconds` at `mbPerSecond`, rounded up to the
    // editor's steps (64, 128, 256, 512, 1024).
    inline std::uint32_t MegabytesFor(std::uint32_t seconds, double mbPerSecond)
    {
        const double need = static_cast<double>(seconds) * mbPerSecond;
        static const std::uint32_t kSteps[] = {64, 128, 256, 512, 1024};
        for (std::uint32_t step : kSteps)
            if (need <= static_cast<double>(step))
                return step;
        return 2048;
    }

    // The part number of a manual save. `partsSaved` is how many parts of this
    // chain were saved before (0: the recording never split, no suffix). The
    // chain's final save is the next part, not the last one.
    inline int FinalPartNumber(int partsSaved)
    {
        return partsSaved > 0 ? partsSaved + 1 : 0;
    }

    // ManualMaxMB's range. A part is checked once a block, so it closes up to a
    // block past its limit (512 MB parts were saved at 514 to 543 MB, the file
    // about the same size); 960 keeps that overshoot under the 1 GiB a clip file
    // may hold (clipcompress::kMaxClipBytes, a larger clip does not open).
    constexpr std::uint32_t kManualMinMB = 32;
    constexpr std::uint32_t kManualMaxMB = 960;

    inline std::uint32_t ClampManualMB(std::uint32_t megabytes)
    {
        return std::clamp(megabytes, kManualMinMB, kManualMaxMB);
    }

    // ReplayBufferMB's range (and the adaptive ReplayBufferMaxMB's): a saved
    // buffer holds up to its budget plus the block that crossed it, so the
    // same 960 keeps a saved buffer under kMaxClipBytes. Before, the ini took
    // any value and a buffer over 1 GiB saved a clip that would not open.
    constexpr std::uint32_t kBufferMinMB = 16;
    constexpr std::uint32_t kBufferMaxMB = kManualMaxMB;

    inline std::uint32_t ClampBufferMB(std::uint32_t megabytes)
    {
        return std::clamp(megabytes, kBufferMinMB, kBufferMaxMB);
    }

    // The size a manual part closes at: ManualMaxMB, or with the buffer on the
    // ring's budget when that is larger (0 without the buffer), never past
    // kManualMaxMB.
    inline std::size_t ManualPartBytes(std::uint32_t manualMaxMB, std::size_t ringBudgetBytes)
    {
        constexpr std::size_t MB = 1024 * 1024;
        const std::size_t manual = static_cast<std::size_t>(ClampManualMB(manualMaxMB)) * MB;
        return (std::min)((std::max)(manual, ringBudgetBytes), static_cast<std::size_t>(kManualMaxMB) * MB);
    }

    // ManualMaxSeconds (0: no time limit), counted from the part's start.
    inline bool ManualTimeUp(std::uint32_t maxSeconds, std::uint32_t nowMs, std::uint32_t partStartMs)
    {
        return maxSeconds > 0 && nowMs >= partStartMs && nowMs - partStartMs >= maxSeconds * 1000u;
    }

    // The ring's head has reached a running manual mark: what it holds from
    // here on is the manual part, not the buffer's own seconds.
    inline bool HeadAtManualMark(bool manualInBuffer, std::uint32_t headMs, std::uint32_t manualStartMs)
    {
        return manualInBuffer && headMs + kBlockMs > manualStartMs;
    }

    // What the ring trims to this step: its own budget while the head is still
    // before the manual mark (the seconds before the mark are the buffer's),
    // the manual part's size (ManualPartBytes) once it reached the mark: over
    // it, the part is saved and the next starts.
    inline std::size_t RingLimitBytes(bool headAtManualMark, std::size_t ringBudgetBytes, std::size_t manualPartBytes)
    {
        return headAtManualMark ? manualPartBytes : ringBudgetBytes;
    }
}
