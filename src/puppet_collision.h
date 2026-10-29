#pragma once

// Collision filter of a walking ped or horse puppet. The puppet must stay an
// active physics body (bushes react only to an active body), but then the
// engine's physics pushes it off the recorded track. So the puppet keeps its
// active instance for the plants and collides with nothing else: its physics
// instance's include flags are restricted to the vegetation and query
// classes, so no pair with the map, peds, horses, vehicles or objects is ever
// formed (a pair needs both sides to include each other). The end-of-frame
// write (mount_placement.h) covers whatever still moves it.
//
// Engine facts (build 1491.50):
// - entity +0x38 = the current physics instance (phInst), +0x08 of it the
//   archetype: _SET_PED_USE_HORSE_MAP_COLLISION's body 0xFF58A4 reads
//   [ped+0x38] (0xFF59B3) and [inst+0x08] (0xFF59BC).
// - inst +0xC0 = type flags (what it is), +0xC8 = include flags (what it
//   collides with): the same function writes include at 0xFF59E1 and type
//   at 0xFF59F6 (0x200 ped capsule, 0x800 horse capsule: `and eax, 0x600;
//   add rax, 0x200`), with the instance in the level and no level call;
//   the level insert 0x2823760 copies archetype +0x28/+0x30 into them
//   (0x282387C..0x282388A).
// - pair filter 0x2847C0C: (A.include & B.type) != 0 and (A.type &
//   B.include) != 0, on per-frame copies of the two fields.
// - class bits from the script shape test flag translation 0x10AFA84 and the
//   Foliage Detector's include 0x80000 (0xC276E6): map 2 and 3, vehicles 6,
//   7, 25, peds 9, horses 11, ragdolls 10 and 12, objects 13, foliage 19.
//   Env cloth 14 and plant 15 (inferred, not proven), query classes 21 to
//   24 (24 proven: the script test type).
//
// Guarded reads of the puppet's entity and instance, guarded writes of one
// u64 on the replay's own puppet's instance (the engine itself writes there
// at 0xFF59E1, 0x1354A84, 0x14C48DC). Never the archetype (shared by every
// ped of the model). Script thread only, no native here.
//
// The pure part (the plan of a tick) is below and unit tested; the reads and
// writes are in puppet_collision.cpp.

#include <cstddef>
#include <cstdint>
#include <iosfwd>

namespace replay::collision
{
    constexpr std::size_t kEntityInstance = 0x38;
    constexpr std::size_t kInstanceLevel = 0x10;   // u32: level index + generation, 0 out of the level
    constexpr std::size_t kInstanceState = 0x14;   // u16, bits 0..1: 0 active, 1 inactive, 2 fixed
    constexpr std::size_t kInstanceType = 0xC0;
    constexpr std::size_t kInstanceInclude = 0xC8;
    constexpr std::uint64_t kTypePedCapsule = 0x200;
    constexpr std::uint64_t kTypeHorseCapsule = 0x800;
    // Env cloth 14, plant 15, foliage 19, weapon / camera / AI / script
    // tests 21..24. AND with the instance's own
    // value: a bit the puppet did not have is never added.
    constexpr std::uint64_t kDefaultKeep = 0x01E8C000ull;

    constexpr bool CapsuleType(std::uint64_t type)
    {
        return type == kTypePedCapsule || type == kTypeHorseCapsule;
    }

    // One puppet's filter as the script thread keeps it.
    struct State
    {
        std::uint64_t instance = 0; // the instance written (0: none)
        std::uint64_t original = 0; // its include before the write
        std::uint64_t written = 0;  // what was written
    };

    enum class Step : std::uint8_t
    {
        None,     // already restricted, nothing to do
        Write,    // first write on this instance
        Rewrite,  // the engine put another include back on the same instance: restrict it again
        NewInstance, // the entity has another instance now (reinsert): restrict the new one
        Refuse,   // not a ped or horse capsule (a ragdoll instance, a garbage read): no write
        Restore,  // put the original back (the body turns frozen)
        Lost,     // nothing to restore: the instance changed or the value is not ours any more
    };

    struct Plan
    {
        Step step = Step::None;
        std::uint64_t value = 0;    // what to write (Write, Rewrite, NewInstance, Restore)
        std::uint64_t original = 0; // the original the state keeps after the step
    };

    // While the body is active, every tick: the instance, its type and its
    // include as read now.
    inline Plan PlanRestrict(const State& state, std::uint64_t instance, std::uint64_t type, std::uint64_t include,
        std::uint64_t keep)
    {
        Plan plan;
        if (!instance || !CapsuleType(type))
        {
            plan.step = Step::Refuse;
            return plan;
        }
        const std::uint64_t restricted = include & keep;
        if (state.instance == instance && include == state.written)
        {
            plan.step = Step::None;
            plan.original = state.original;
            return plan;
        }
        if (state.instance == instance)
        {
            // Someone wrote the include again: their value is the new
            // original (it already is, when it is the old original back).
            plan.step = Step::Rewrite;
            plan.original = include;
            plan.value = restricted;
            return plan;
        }
        plan.step = state.instance ? Step::NewInstance : Step::Write;
        plan.original = include;
        plan.value = restricted;
        return plan;
    }

    // The body turns frozen (or the playback ends with it alive).
    inline Plan PlanRestore(const State& state, std::uint64_t instance, std::uint64_t include)
    {
        Plan plan;
        if (!state.instance)
        {
            plan.step = Step::None;
            return plan;
        }
        if (instance != state.instance || include != state.written)
        {
            plan.step = Step::Lost;
            return plan;
        }
        plan.step = Step::Restore;
        plan.value = state.original;
        plan.original = state.original;
        return plan;
    }

    inline const char* StepText(Step step)
    {
        switch (step)
        {
        case Step::None: return "none";
        case Step::Write: return "write";
        case Step::Rewrite: return "rewrite";
        case Step::NewInstance: return "new_instance";
        case Step::Refuse: return "refuse";
        case Step::Restore: return "restore";
        case Step::Lost: return "lost";
        }
        return "unknown";
    }

    // ------------------------------------------------------------------
    // Mod part (puppet_collision.cpp, ReplayProbe only). Script thread.

    // PhysicsFilter in the ini (default: JitterFix, 1) and PuppetIncludeKeep
    // (default kDefaultKeep). Off: the body collides with the world, the
    // other puppets kept off pair by pair.
    void Configure(bool on, std::uint64_t keep);
    bool Enabled();
    std::uint64_t Keep();
    // Every tick the puppet's body is active, after the activation natives:
    // restricts (or keeps restricted) its instance. True while restricted.
    bool Apply(void* entity, std::uint32_t packed, std::ostream& log);
    // Before the freeze natives: the original include back on the same
    // instance (nothing when it is not ours any more).
    void Release(void* entity, std::uint32_t packed, std::ostream& log);
    // The puppet is deleted (its address may come back as another entity).
    void Forget(void* entity);
    // Playback end: the totals line, then everything forgotten.
    void EndPlayback(std::ostream& log);
}
