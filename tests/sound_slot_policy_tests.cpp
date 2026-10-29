// The playback slot policy of the entity sounds (sound_slot_policy.h).
#include "sound_slot_policy.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    using namespace replay::soundslots;

    int g_slotFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_slotFailures;
    }

    SlotView Used(std::uint32_t owner, float distance, std::uint32_t startedMs)
    {
        SlotView slot;
        slot.used = true;
        slot.owner = owner;
        slot.distance = distance;
        slot.startedMs = startedMs;
        return slot;
    }

    void TestFree()
    {
        std::vector<SlotView> slots(4);
        slots[0] = Used(1, 5.0f, 100);
        const Choice choice = Choose(slots.data(), slots.size(), 2, 10.0f);
        Check(choice.slot == 1 && choice.reason == Reason::Free && !choice.Steals(), "slots: first free slot");
        Check(Choose(nullptr, 0, 2, 10.0f).slot == -1, "slots: no slots refuses");
    }

    void TestTooFar()
    {
        std::vector<SlotView> slots(4);
        const Choice far = Choose(slots.data(), slots.size(), 1, kMaxDistanceM + 1.0f);
        Check(far.slot == -1 && far.reason == Reason::TooFar, "slots: beyond the audible distance is not started");
        Check(Choose(slots.data(), slots.size(), 1, NAN).reason == Reason::TooFar, "slots: NaN distance refused");
        Check(Choose(slots.data(), slots.size(), 1, kMaxDistanceM).reason == Reason::Free, "slots: at the limit starts");
    }

    void TestOwnerCap()
    {
        std::vector<SlotView> slots(kPerOwnerCap + 4);
        for (std::uint32_t index = 0; index < kPerOwnerCap; ++index)
            slots[index] = Used(7, 3.0f, 300 + index * 100);
        slots[1] = Used(7, 3.0f, 100); // the owner's oldest
        const Choice choice = Choose(slots.data(), slots.size(), 7, 3.0f);
        Check(choice.slot == 1 && choice.reason == Reason::OwnerCap && choice.Steals(),
            "slots: an owner at its cap replaces its own oldest even with free slots");
        const Choice other = Choose(slots.data(), slots.size(), 8, 3.0f);
        Check(other.slot == static_cast<int>(kPerOwnerCap) && other.reason == Reason::Free, "slots: another owner still gets a free slot");
        Check(Choose(slots.data(), slots.size(), 7, 3.0f, 0).reason == Reason::Free, "slots: cap 0 disables the cap");
        // The oldest is a loop (the horse's snow bed): the oldest
        // short sound of the owner goes instead; all long: the oldest still.
        slots[1].longSound = true;
        const Choice keepLoop = Choose(slots.data(), slots.size(), 7, 3.0f);
        Check(keepLoop.slot == 0 && keepLoop.reason == Reason::OwnerCap, "slots: the cap takes the oldest short sound, not the loop");
        for (std::uint32_t index = 0; index < kPerOwnerCap; ++index)
            slots[index].longSound = true;
        Check(Choose(slots.data(), slots.size(), 7, 3.0f).slot == 1, "slots: every sound long: the oldest");
    }

    void TestStealFarthest()
    {
        std::vector<SlotView> slots(3);
        slots[0] = Used(1, 10.0f, 100);
        slots[1] = Used(2, 50.0f, 300); // the farthest
        slots[2] = Used(3, 20.0f, 50);
        const Choice choice = Choose(slots.data(), slots.size(), 4, 15.0f);
        Check(choice.slot == 1 && choice.reason == Reason::Farther, "slots: full, the farthest is replaced by a nearer one");
        const Choice refused = Choose(slots.data(), slots.size(), 4, 60.0f);
        Check(refused.slot == -1 && refused.reason == Reason::Full, "slots: full and every sound nearer refuses");
    }

    void TestStealOldestAtSameDistance()
    {
        std::vector<SlotView> slots(3);
        slots[0] = Used(1, 30.0f, 500);
        slots[1] = Used(2, 30.4f, 100); // as far (within kSameDistanceM) and the oldest
        slots[2] = Used(3, 29.8f, 300);
        const Choice choice = Choose(slots.data(), slots.size(), 4, 30.0f);
        Check(choice.slot == 1 && choice.reason == Reason::Farther, "slots: as far, the oldest is replaced");
    }

    void TestTrainCrowd()
    {
        // The train clip: 60 passengers 2 to 40 m away, 5 foley sounds each,
        // through 48 slots. The cap keeps any owner at 4, the nearest win.
        std::vector<SlotView> slots(48);
        std::uint32_t refused = 0;
        std::uint32_t time = 0;
        for (std::uint32_t round = 0; round < 5; ++round)
            for (std::uint32_t owner = 1; owner <= 60; ++owner)
            {
                const float distance = 2.0f + static_cast<float>(owner) * 0.65f;
                const Choice choice = Choose(slots.data(), slots.size(), owner, distance);
                if (choice.slot < 0)
                {
                    ++refused;
                    continue;
                }
                slots[static_cast<std::size_t>(choice.slot)] = Used(owner, distance, ++time);
            }
        std::uint32_t perOwner[61] = {};
        float farthest = 0.0f;
        for (const SlotView& slot : slots)
            if (slot.used)
            {
                ++perOwner[slot.owner];
                farthest = slot.distance > farthest ? slot.distance : farthest;
            }
        bool capped = true;
        for (std::uint32_t owner = 1; owner <= 60; ++owner)
            capped = capped && perOwner[owner] <= kPerOwnerCap;
        Check(capped, "slots: crowd, no owner above the cap");
        Check(perOwner[1] > 0 && perOwner[60] == 0, "slots: crowd, the nearest owners keep sounds, the farthest lose them");
        Check(refused > 0 && farthest < 2.0f + 60.0f * 0.65f, "slots: crowd, far sounds refused, none of the farthest kept");
    }

    // The generic player's cap key. A world packet counts with its own sound.
    constexpr std::uint32_t kNoOwner = 0xFFFF0000u; // census::kNoOwner

    SlotView Keyed(std::uint32_t owner, std::uint64_t soundKey, bool perSound, std::uint32_t startedMs, bool longSound,
        float distance = 5.0f)
    {
        SlotView slot = Used(owner, distance, startedMs);
        slot.capKey = CapKeyOf(owner, kNoOwner, soundKey, perSound);
        slot.longSound = longSound;
        return slot;
    }

    void TestCapKeyOf()
    {
        Check(CapKeyOf(0x00070008u, kNoOwner, 0xd9388, true) == 0x00070008u, "cap key: an owned sound counts with its owner");
        Check(CapKeyOf(kNoOwner, kNoOwner, 0xd9388, false) == kNoOwner, "cap key: the shared world key is the no-owner id");
        const std::uint64_t world = CapKeyOf(kNoOwner, kNoOwner, 0xd9388, true);
        Check(IsWorldCapKey(world) && (world & ~kWorldCapKeyBit) == 0xd9388, "cap key: a world sound counts with its sound");
        Check(CapKeyOf(kNoOwner, kNoOwner, 0x4c4a0c, true) != world, "cap key: two world sounds have two keys");
        Check(!IsWorldCapKey(CapKeyOf(0xFFFFFFFFu, kNoOwner, 0xd9388, true)), "cap key: an owner id is never a world key");
        Check(CapKeyOf(kNoOwner, kNoOwner, 0x800002002f49acull, true) != CapKeyOf(kNoOwner, kNoOwner, 0x800004002f49acull, true),
            "cap key: the sound key keeps the entry");
    }

    void TestWorldCapPerSound()
    {
        // Seven world packets of seven sounds: none takes another's slot.
        std::vector<SlotView> slots(16);
        std::uint32_t time = 0;
        for (std::uint64_t id = 1; id <= 7; ++id)
        {
            const std::uint64_t key = CapKeyOf(kNoOwner, kNoOwner, 0x100 + id, true);
            const Choice c = ChooseByKey(slots.data(), slots.size(), key, 5.0f);
            Check(c.reason == Reason::Free && !c.Steals(), "world cap: different sounds never steal from each other");
            if (c.slot >= 0)
                slots[static_cast<std::size_t>(c.slot)] = Keyed(kNoOwner, 0x100 + id, true, ++time, true);
        }
        // Seven of one sound: the seventh takes that sound's oldest short instance.
        std::vector<SlotView> same(16);
        for (std::uint32_t n = 0; n < kPerOwnerCap; ++n)
            same[n] = Keyed(kNoOwner, 0x4c4a0c, true, 100 + n * 10, n != 2); // instance 2 short
        const std::uint64_t gust = CapKeyOf(kNoOwner, kNoOwner, 0x4c4a0c, true);
        const Choice c = ChooseByKey(same.data(), same.size(), gust, 5.0f);
        Check(c.slot == 2 && c.reason == Reason::OwnerCap, "world cap: the seventh of one sound takes its oldest short instance");
        same[2].longSound = true;
        const Choice d = ChooseByKey(same.data(), same.size(), gust, 5.0f);
        Check(d.slot == 0 && d.reason == Reason::OwnerCap, "world cap: all long: the oldest instance");
        Check(ChooseByKey(same.data(), same.size(), gust, 5.0f, 0).reason == Reason::Free, "world cap: count 0 turns the cap off");
        // Owned packets: ChooseByKey with owner keys is Choose.
        std::vector<SlotView> owned(kPerOwnerCap + 2);
        for (std::uint32_t n = 0; n < kPerOwnerCap; ++n)
            owned[n] = Keyed(7, 0x99, true, 300 + n * 100, false);
        const Choice a = ChooseByKey(owned.data(), owned.size(), CapKeyOf(7, kNoOwner, 0x99, true), 3.0f);
        const Choice b = Choose(owned.data(), owned.size(), 7, 3.0f);
        Check(a.slot == b.slot && a.reason == b.reason && a.reason == Reason::OwnerCap, "world cap: owned packets keep the owner cap");
    }

    void TestGallopLoop1152()
    {
        // A horse group's gallop loop (0xd9388, ~20 s) starts, then gusts of
        // 0x4c4a0c (~5 s each, long too) come six at a time. With one shared
        // world key the loop is the oldest world sound and goes at the sixth
        // gust; with a key per sound the loop stays.
        for (int perSound = 0; perSound < 2; ++perSound)
        {
            std::vector<SlotView> slots(64);
            slots[0] = Keyed(kNoOwner, 0xd9388, perSound != 0, 5314, true);
            bool loopStolen = false;
            std::uint32_t time = 5400;
            for (int gust = 0; gust < 12; ++gust)
            {
                time += 400;
                const std::uint64_t key = CapKeyOf(kNoOwner, kNoOwner, 0x4c4a0c, perSound != 0);
                const Choice c = ChooseByKey(slots.data(), slots.size(), key, 8.0f);
                if (c.slot < 0)
                    continue;
                if (c.Steals() && c.slot == 0)
                    loopStolen = true;
                slots[static_cast<std::size_t>(c.slot)] = Keyed(kNoOwner, 0x4c4a0c, perSound != 0, time, true);
            }
            if (perSound)
                Check(!loopStolen && slots[0].capKey == CapKeyOf(kNoOwner, kNoOwner, 0xd9388, true),
                    "world cap 1152: the gallop loop keeps its slot through the gusts");
            else
                Check(loopStolen, "world cap 1152: the shared key loses the gallop loop (the old bug)");
        }
    }
}

int RunSoundSlotPolicyTests()
{
    g_slotFailures = 0;
    TestFree();
    TestTooFar();
    TestOwnerCap();
    TestStealFarthest();
    TestStealOldestAtSameDistance();
    TestTrainCrowd();
    TestCapKeyOf();
    TestWorldCapPerSound();
    TestGallopLoop1152();
    std::printf("  sound slots: %d failed\n", g_slotFailures);
    if (g_slotFailures == 0)
        std::printf("Sound slot policy tests passed.\n");
    return g_slotFailures;
}
