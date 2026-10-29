#pragma once

// The persistent state rebuild after a jump (the blood records of every
// puppet) as a queue run under a time budget per tick, as a replay editor
// budgets its playback. Paid in one tick, the release of a scrub or a far jump
// costs 250 to 1550 ms with every record of every ped: the game does not draw,
// so the spinner behind the frozen picture never shows. Pure so the core tests
// cover it.
//
// - A jump builds a plan per ped: only the difference to the target (the
//   records after the last applied one going forward; clear and rebuild only
//   when the ped holds a record past the target or the puppet is new).
// - Run() takes operations in order while the budget lasts, at least one per
//   call so the queue always drains.
// - The seek settle waits for the queue (Readiness::statePending), so the
//   state the wait ends on is the same as the one-tick rebuild made.

#include <cstddef>
#include <cstdint>
#include <deque>

namespace replay::budget
{
    // Microseconds of rebuild per tick: normal play keeps the frame, the
    // settle holds the time (the scene still draws), the frozen picture
    // only needs the spinner to keep turning.
    constexpr std::uint32_t kPlayUs = 4000;
    constexpr std::uint32_t kSettleUs = 8000;
    constexpr std::uint32_t kFrozenUs = 25000;

    inline std::uint32_t StateBudgetUs(bool settling, bool frozen)
    {
        return frozen ? kFrozenUs : settling ? kSettleUs : kPlayUs;
    }

    struct Op
    {
        enum class Kind : std::uint8_t
        {
            Clear, // remove every record of the ped
            Apply, // lay one recorded record
        };
        Kind kind = Kind::Apply;
        std::uint32_t entity = 0; // packed replay id
        std::size_t index = 0;    // record index in the clip (Apply)
    };

    struct RunStats
    {
        std::uint32_t done = 0;
        std::uint32_t clears = 0;
        std::uint64_t spentUs = 0;
        bool exhausted = false; // stopped by the budget with work left
    };

    class OpQueue
    {
    public:
        void Reset() { m_ops.clear(); }
        bool Empty() const { return m_ops.empty(); }
        std::size_t Size() const { return m_ops.size(); }
        void PushClear(std::uint32_t entity) { m_ops.push_back({Op::Kind::Clear, entity, 0}); }
        void PushApply(std::uint32_t entity, std::size_t index) { m_ops.push_back({Op::Kind::Apply, entity, index}); }
        bool Holds(std::uint32_t entity) const
        {
            for (const Op& op : m_ops)
                if (op.entity == entity)
                    return true;
            return false;
        }

        // nowUs(): a monotonic clock in microseconds; run(op) does the op.
        // Stops when budgetUs is spent, after at least one op.
        template <typename Clock, typename Runner>
        RunStats Run(std::uint32_t budgetUs, Clock nowUs, Runner run)
        {
            RunStats stats;
            const std::uint64_t start = nowUs();
            while (!m_ops.empty())
            {
                if (stats.done > 0 && nowUs() - start >= budgetUs)
                {
                    stats.exhausted = true;
                    break;
                }
                const Op op = m_ops.front();
                m_ops.pop_front();
                run(op);
                ++stats.done;
                if (op.kind == Op::Kind::Clear)
                    ++stats.clears;
            }
            stats.spentUs = nowUs() - start;
            return stats;
        }

    private:
        std::deque<Op> m_ops;
    };

    // What a ped needs to reach the target from what it holds.
    enum class PedPlan : std::uint8_t
    {
        Nothing, // holds exactly the records up to the target
        Forward, // lay the records in (appliedUpTo, target]
        Rebuild, // clear, then lay every record up to the target
    };

    // hasApplied/appliedUpTo: the time of the last record laid on this ped
    // (records go on in time order, so it holds every record up to it);
    // sameClone: that was on the puppet alive now; lastDue: the time of the
    // last record at or before the target (anyDue false: none).
    inline PedPlan PlanPed(bool hasApplied, std::uint32_t appliedUpTo, bool sameClone, bool anyDue, std::uint32_t lastDue,
        std::uint32_t targetMs)
    {
        const bool tooMuch = hasApplied && appliedUpTo > targetMs;
        const bool missing = anyDue && (!hasApplied || appliedUpTo < lastDue);
        if (!tooMuch && !missing)
            return PedPlan::Nothing;
        if (hasApplied && !tooMuch && sameClone)
            return PedPlan::Forward;
        return PedPlan::Rebuild;
    }
}
