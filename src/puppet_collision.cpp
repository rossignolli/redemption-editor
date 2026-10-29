// Reads and writes of the puppet physics filter (puppet_collision.h).
// Script thread only; every access guarded.

#include "puppet_collision.h"

#include "guarded_memory.h"
#include "log_policy.h"
#include "entity_lifecycle.h"

#include <cstdio>
#include <ostream>
#include <unordered_map>

namespace replay::collision
{
    namespace
    {
        bool g_enabled = true;
        std::uint64_t g_keep = kDefaultKeep;
        std::unordered_map<std::uint64_t, State> g_states; // by the puppet's engine address

        struct Counts
        {
            std::uint64_t writes = 0;
            std::uint64_t rewrites = 0;
            std::uint64_t newInstances = 0;
            std::uint64_t restores = 0;
            std::uint64_t lost = 0;
            std::uint64_t refused = 0;
            std::uint64_t readFail = 0;
            std::uint64_t writeFail = 0;
            std::uint64_t readbackFail = 0;
            std::uint64_t activeTicks = 0;
            std::uint64_t firstInclude = 0; // evidence: the first original seen, and its type
            std::uint64_t firstType = 0;
        } g_counts;
        replay::log::LogLimit g_lines;

        struct Read
        {
            std::uint64_t instance = 0;
            std::uint64_t type = 0;
            std::uint64_t include = 0;
            std::uint32_t level = 0;
            std::uint16_t state = 0;
        };

        bool ReadInstance(const void* entity, Read& out)
        {
            const auto base = reinterpret_cast<std::uintptr_t>(entity);
            if (!replay::mem::Read(base + kEntityInstance, out.instance))
                return false;
            if (!out.instance)
                return true; // no instance: PlanRestrict refuses
            const auto inst = static_cast<std::uintptr_t>(out.instance);
            return replay::mem::Read(inst + kInstanceType, out.type) && replay::mem::Read(inst + kInstanceInclude, out.include) &&
                replay::mem::Read(inst + kInstanceLevel, out.level) && replay::mem::Read(inst + kInstanceState, out.state);
        }

        // Rung 3: one u64 on the puppet's own instance, read back.
        bool WriteInclude(std::uint64_t instance, std::uint64_t value)
        {
            const auto target = static_cast<std::uintptr_t>(instance) + kInstanceInclude;
            if (!replay::mem::Put(target, &value, sizeof(value)))
            {
                ++g_counts.writeFail;
                return false;
            }
            std::uint64_t back = 0;
            if (!replay::mem::Read(target, back) || back != value)
            {
                ++g_counts.readbackFail;
                return false;
            }
            return true;
        }

        void Line(std::ostream& log, std::uint32_t packed, Step step, const Read& read, std::uint64_t value)
        {
            if (!g_lines.Take(24))
                return;
            char text[256]{};
            std::snprintf(text, sizeof(text),
                "POSE_V2_COLLISION_FILTER,id=%s,step=%s,type=0x%llx,include=0x%llx,written=0x%llx,keep=0x%llx,level=0x%x,"
                "state=%u\n",
                replay::entity::IdText(replay::entity::ReplayId::FromPacked(packed)).c_str(), StepText(step),
                static_cast<unsigned long long>(read.type), static_cast<unsigned long long>(read.include),
                static_cast<unsigned long long>(value), static_cast<unsigned long long>(g_keep), read.level,
                static_cast<unsigned>(read.state & 3u));
            log << text;
        }
    }

    void Configure(bool on, std::uint64_t keep)
    {
        g_enabled = on;
        g_keep = keep;
    }

    bool Enabled() { return g_enabled; }
    std::uint64_t Keep() { return g_keep; }

    bool Apply(void* entity, std::uint32_t packed, std::ostream& log)
    {
        if (!g_enabled || !entity)
            return false;
        ++g_counts.activeTicks;
        Read read;
        if (!ReadInstance(entity, read))
        {
            ++g_counts.readFail;
            return false;
        }
        State& state = g_states[reinterpret_cast<std::uint64_t>(entity)];
        const Plan plan = PlanRestrict(state, read.instance, read.type, read.include, g_keep);
        switch (plan.step)
        {
        case Step::None:
            return true;
        case Step::Refuse:
            ++g_counts.refused;
            Line(log, packed, plan.step, read, 0);
            return false;
        default:
            break;
        }
        if (!g_counts.firstType)
        {
            g_counts.firstType = read.type;
            g_counts.firstInclude = read.include;
        }
        if (!WriteInclude(read.instance, plan.value))
        {
            state = State{};
            Line(log, packed, plan.step, read, plan.value);
            return false;
        }
        g_counts.writes += plan.step == Step::Write ? 1 : 0;
        g_counts.rewrites += plan.step == Step::Rewrite ? 1 : 0;
        g_counts.newInstances += plan.step == Step::NewInstance ? 1 : 0;
        state.instance = read.instance;
        state.original = plan.original;
        state.written = plan.value;
        Line(log, packed, plan.step, read, plan.value);
        return true;
    }

    void Release(void* entity, std::uint32_t packed, std::ostream& log)
    {
        if (!entity)
            return;
        const auto found = g_states.find(reinterpret_cast<std::uint64_t>(entity));
        if (found == g_states.end())
            return;
        Read read;
        const State state = found->second;
        g_states.erase(found);
        if (!ReadInstance(entity, read))
        {
            ++g_counts.readFail;
            return;
        }
        const Plan plan = PlanRestore(state, read.instance, read.include);
        if (plan.step == Step::Lost)
        {
            ++g_counts.lost;
            Line(log, packed, plan.step, read, 0);
            return;
        }
        if (plan.step == Step::Restore && WriteInclude(read.instance, plan.value))
            ++g_counts.restores;
    }

    void Forget(void* entity)
    {
        g_states.erase(reinterpret_cast<std::uint64_t>(entity));
    }

    void EndPlayback(std::ostream& log)
    {
        if (g_counts.activeTicks || g_counts.writes || g_counts.refused)
        {
            char text[512]{};
            std::snprintf(text, sizeof(text),
                "POSE_V2_COLLISION_FILTER_SUMMARY,enabled=%d,keep=0x%llx,active_ticks=%llu,writes=%llu,rewrites=%llu,"
                "new_instances=%llu,restores=%llu,lost=%llu,refused=%llu,read_fail=%llu,write_fail=%llu,"
                "readback_fail=%llu,first_type=0x%llx,first_include=0x%llx\n",
                g_enabled ? 1 : 0, static_cast<unsigned long long>(g_keep),
                static_cast<unsigned long long>(g_counts.activeTicks), static_cast<unsigned long long>(g_counts.writes),
                static_cast<unsigned long long>(g_counts.rewrites), static_cast<unsigned long long>(g_counts.newInstances),
                static_cast<unsigned long long>(g_counts.restores), static_cast<unsigned long long>(g_counts.lost),
                static_cast<unsigned long long>(g_counts.refused), static_cast<unsigned long long>(g_counts.readFail),
                static_cast<unsigned long long>(g_counts.writeFail), static_cast<unsigned long long>(g_counts.readbackFail),
                static_cast<unsigned long long>(g_counts.firstType), static_cast<unsigned long long>(g_counts.firstInclude));
            log << text;
        }
        g_states.clear();
        g_counts = Counts{};
    }
}
