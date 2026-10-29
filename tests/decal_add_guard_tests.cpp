// The checks before a decal reaches the rage decal core (decal_add_guard.h).
#include "decal_add_guard.h"

#include <cstdio>

namespace
{
    using namespace replay::decal::guard;

    int g_guardFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_guardFailures;
    }

    void TestIds()
    {
        Check(IdSlot(0) == -1, "id 0 has no slot");
        Check(IdSlot(0xFFFF) == -1, "an id below 0x10000 has no slot");
        Check(IdSlot(0x10005) == 0, "first slot");
        Check(IdSlot(0x0C000001) == 0xBFF, "last slot of the table");
        Check(IdSlot(0x0C010001) == -1, "one past the table");
        Check(IdSlot(0x4BD10000) == -1, "the slot that lands on a ring entry's +0x318");
        Check(IdAlive(0x00030007, 7), "generation matches the low word");
        Check(!IdAlive(0x00030007, 8), "freed: generation moved on");
        Check(!IdAlive(0x0C010001, 1), "out of the table is never alive");
    }

    void TestInfo()
    {
        Check(JudgeInfo(0, false, 0) == InfoVerdict::Null, "null info");
        Check(JudgeInfo(0x20, true, 1) == InfoVerdict::Unreadable, "tiny pointer");
        Check(JudgeInfo(0x7FF600001000ull, false, 0) == InfoVerdict::Unreadable, "unreadable record");
        Check(JudgeInfo(0x7FF600001000ull, true, 0) == InfoVerdict::NoData, "+0x130 zero: the game refuses it");
        Check(JudgeInfo(0x7FF600001000ull, true, 0x1234) == InfoVerdict::Ok, "a good record");
        Refusals refusals;
        Count(refusals, InfoVerdict::Null);
        Count(refusals, InfoVerdict::NoData);
        Count(refusals, InfoVerdict::Ok);
        refusals.ringStale = 2;
        Check(refusals.Info() == 2 && refusals.Total() == 4, "refusal totals");
    }

    void TestRing()
    {
        Check(NextRingIndex(0) == 1, "next entry");
        Check(NextRingIndex(0x3FF) == 0, "wraps like the core");
        Check(RingEntityAddress(0x1000000, 2) == 0x1000000 + 0xB5D0 + 2 * 0x330 + 0x318, "entry entity address");
        Check(RingRefPlausible(0, false), "an empty entry is fine");
        Check(!RingRefPlausible(1, false), "previous entity 1");
        Check(!RingRefPlausible(0x1F00000000ull + 4, true), "misaligned pointer");
        Check(!RingRefPlausible(0x1F00000000ull, false), "unreadable pointer");
        Check(RingRefPlausible(0x1F00000000ull, true), "a live entity");
    }
}

int RunDecalAddGuardTests()
{
    g_guardFailures = 0;
    TestIds();
    TestInfo();
    TestRing();
    if (!g_guardFailures)
        std::printf("Decal add guard tests passed.\n");
    return g_guardFailures;
}
