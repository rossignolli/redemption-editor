#pragma once

// Which playback slot a replayed persistent sound gets (pure, unit tested in
// tests/sound_slot_policy_tests.cpp).
//
// The entity sound track (ESND: object loops, ped and train car audio) plays
// through a fixed set of slots this module owns. Without a policy a train in
// town fills all 48 and refuses the rest: every passenger's foley holds a slot
// for its whole recorded span. A bounded pool of tracked sounds lets the far
// and old ones go; the rules:
//
// - a sound farther than maxDistance from the listener (the rendered camera)
//   is not started (it would not be heard);
// - one owner holds at most perOwnerCap slots (6: the ped sound controller starts
//   up to five variants of one foley sound in the same frame): past it the owner's oldest
//   sound gives its slot to the new one;
// - a free slot is taken first (a slot frees itself when the engine ends
//   its sound: it clears the slot);
// - with none free, the farthest playing sound is replaced when it is
//   farther than the new one (the oldest among equally far ones); otherwise
//   the new one is refused.
//
// The cap counts by a key (ChooseByKey, CapKeyOf). An owned sound's key is its
// owner (the rule above); a world sound (no owner: the generic player's
// packets of census::kNoOwner) is keyed by its sound, not by "nobody". Shared
// by one "nobody" owner, the 6 slots would be fought over by the horses' group
// gallop loop (0xd9388), the ambient zone loops and the wind gusts (0x4c4a0c),
// and a loop would die seconds after it started.

#include <cstddef>
#include <cstdint>

namespace replay::soundslots
{
    constexpr std::uint32_t kPerOwnerCap = 6; // a ped controller starts up to 5 layers of one foley at once
    constexpr float kMaxDistanceM = 120.0f;
    constexpr float kSameDistanceM = 1.0f;

    struct SlotView
    {
        bool used = false;
        std::uint32_t owner = 0;   // packed replay id
        float distance = 0.0f;     // to the listener, metres
        std::uint32_t startedMs = 0;
        // A long sound (a loop, or a recorded life of kLongSoundMs or more):
        // the owner cap takes the owner's oldest short sound first, because a
        // stolen loop never comes back.
        bool longSound = false;
        // The key ChooseByKey counts the cap by (CapKeyOf); Choose ignores it
        // and counts by owner.
        std::uint64_t capKey = 0;
    };
    constexpr std::uint32_t kLongSoundMs = 2000;

    // The cap key of a sound. An owned sound (and every sound when
    // worldPerSound is off) counts with its owner; a sound of no owner counts
    // with the other instances of
    // the same sound (soundKey: soundowner::PacketSoundKey). The world keys
    // carry bit 63, so they never meet an owner's (a packed id is 32 bits).
    constexpr std::uint64_t kWorldCapKeyBit = 1ull << 63;
    inline std::uint64_t CapKeyOf(std::uint32_t owner, std::uint32_t noOwner, std::uint64_t soundKey, bool worldPerSound)
    {
        if (worldPerSound && owner == noOwner)
            return kWorldCapKeyBit | (soundKey & ~kWorldCapKeyBit);
        return owner;
    }
    inline bool IsWorldCapKey(std::uint64_t key) { return (key & kWorldCapKeyBit) != 0; }

    enum class Reason : std::uint8_t
    {
        Free,     // a free slot
        OwnerCap, // replaces the owner's oldest sound
        Farther,  // replaces a farther (or as far and older) sound
        TooFar,   // refused: beyond maxDistance
        Full,     // refused: every playing sound is nearer
    };

    struct Choice
    {
        int slot = -1; // -1: refused
        Reason reason = Reason::Full;
        bool Steals() const { return reason == Reason::OwnerCap || reason == Reason::Farther; }
    };

    inline const char* ReasonName(Reason reason)
    {
        switch (reason)
        {
        case Reason::Free: return "free";
        case Reason::OwnerCap: return "owner_cap";
        case Reason::Farther: return "farther";
        case Reason::TooFar: return "too_far";
        case Reason::Full: return "full";
        }
        return "?";
    }

    // b plays "worse" than a: farther, or as far and older.
    inline bool Worse(const SlotView& a, const SlotView& b)
    {
        if (b.distance > a.distance + kSameDistanceM)
            return true;
        if (a.distance > b.distance + kSameDistanceM)
            return false;
        return b.startedMs < a.startedMs;
    }

    namespace detail
    {
        // The rule, with `same` saying which slots count toward the cap.
        template <typename Same>
        inline Choice ChooseWith(const SlotView* slots, std::size_t count, float distance, std::uint32_t cap,
            float maxDistance, Same same)
        {
            Choice choice;
            if (!(distance <= maxDistance)) // NaN refused too
            {
                choice.reason = Reason::TooFar;
                return choice;
            }
            std::uint32_t owned = 0;
            int oldestOwned = -1;
            int oldestShort = -1;
            int free = -1;
            int worst = -1;
            for (std::size_t index = 0; index < count; ++index)
            {
                const SlotView& slot = slots[index];
                if (!slot.used)
                {
                    if (free < 0)
                        free = static_cast<int>(index);
                    continue;
                }
                if (same(slot))
                {
                    ++owned;
                    if (oldestOwned < 0 || slot.startedMs < slots[oldestOwned].startedMs)
                        oldestOwned = static_cast<int>(index);
                    if (!slot.longSound && (oldestShort < 0 || slot.startedMs < slots[oldestShort].startedMs))
                        oldestShort = static_cast<int>(index);
                }
                if (worst < 0 || Worse(slots[worst], slot))
                    worst = static_cast<int>(index);
            }
            if (cap && owned >= cap && oldestOwned >= 0)
            {
                choice.slot = oldestShort >= 0 ? oldestShort : oldestOwned;
                choice.reason = Reason::OwnerCap;
                return choice;
            }
            if (free >= 0)
            {
                choice.slot = free;
                choice.reason = Reason::Free;
                return choice;
            }
            SlotView incoming;
            incoming.used = true;
            incoming.distance = distance;
            incoming.startedMs = 0xFFFFFFFFu; // the newest
            if (worst >= 0 && Worse(incoming, slots[worst]))
            {
                choice.slot = worst;
                choice.reason = Reason::Farther;
                return choice;
            }
            choice.reason = Reason::Full;
            return choice;
        }
    }

    // The cap counts the slots of the same owner (the entity sound track).
    inline Choice Choose(const SlotView* slots, std::size_t count, std::uint32_t owner, float distance,
        std::uint32_t perOwnerCap = kPerOwnerCap, float maxDistance = kMaxDistanceM)
    {
        return detail::ChooseWith(slots, count, distance, perOwnerCap, maxDistance,
            [owner](const SlotView& slot) { return slot.owner == owner; });
    }

    // The cap counts the slots of the same cap key (CapKeyOf; the generic
    // sound player).
    inline Choice ChooseByKey(const SlotView* slots, std::size_t count, std::uint64_t capKey, float distance,
        std::uint32_t perKeyCap = kPerOwnerCap, float maxDistance = kMaxDistanceM)
    {
        return detail::ChooseWith(slots, count, distance, perKeyCap, maxDistance,
            [capKey](const SlotView& slot) { return slot.capKey == capKey; });
    }
}
