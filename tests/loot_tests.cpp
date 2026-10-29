// The loot bench's rules: drawers, chests and picked
// items replay as live (no closed original next to the open replay drawer, no
// item both on the shelf and in the hand), posed objects share the capture
// slots the bodies leave, a map dressing object removed within reach is a
// take, and the drawer within reach is read every tick.
#include "capture_capacity.h"
#include "object_check.h"
#include "object_pose.h"

#include <cstdint>
#include <cstdio>

namespace
{
    using namespace replay::objcheck;

    int g_lootFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_lootFailures;
    }

    void TestTakenDressing()
    {
        // 1212 and 1213: the drawer and the shelf item were map dressing; a
        // prop took them over. Left to the map, the original stayed drawn.
        Check(MapDressingVerdict(true, false, true, true, 0) == MapDressing::Build,
            "taken, original there: built (the original hidden), never left drawn");
        Check(MapDressingVerdict(true, false, true, false, kMapDressingWaitMs) == MapDressing::Build,
            "taken, original not found yet: built");
        Check(MapDressingVerdict(false, true, true, true, 0) == MapDressing::Build, "visibility changed: built");
        Check(MapDressingVerdict(false, false, true, true, 0) == MapDressing::Live, "untouched and there: the map's");
    }

    void TestDressingRemoval()
    {
        // Removed within reach of a recorded ped with the player close: taken.
        Check(DressingRemovalOf(0.8f, 0.8f), "removed at arm's length: a take");
        Check(DressingRemovalOf(kLootReachM, kLootGonePlayerM), "the limits are a take");
        // Streamed out: far from every ped, or the player far.
        Check(!DressingRemovalOf(25.0f, 25.0f), "removed far away: streamed out");
        Check(!DressingRemovalOf(1.0f, kLootGonePlayerM + 1.0f), "the player far: streamed out");
        Check(!DressingRemovalOf(-1.0f, 1.0f), "no recorded ped read: streamed out");
        Check(!DressingRemovalOf(1.0f, -1.0f), "the player unread: streamed out");
    }

    void TestNearWatch()
    {
        Check(NearWatch(0.0f) && NearWatch(kSkeletonNearM), "within reach: read every tick");
        Check(!NearWatch(kSkeletonNearM + 0.1f) && !NearWatch(-1.0f), "out of reach or unread: four times a second");
        Check(OpenAtStart(RestState::Away), "a drawer left open at the snapshot is recorded at once");
        Check(!OpenAtStart(RestState::AtRest) && !OpenAtStart(RestState::Unknown), "at rest or unknown: the old rule");
    }

    void TestSharedPlaces()
    {
        using namespace replay::object_pose;
        namespace cap = replay::capacity;
        // The slots the two body classes leave, at the GTA world model.
        const std::uint32_t shared =
            WeaponPlaces(cap::kLiveTargetSlots, cap::LiveTargets(cap::kGtaDefaultPedCapacity, cap::kMaxVehicleCapacity));
        Check(shared == 100, "100 shared slots at 148 peds and 79 vehicles");
        // 1211-Blackwater: four bags of the town's NPCs held the four places;
        // the drawer that opened later had none (no_place). Now it starts.
        Check(ShouldPoseShared(false, 16, 4, 0, shared), "the drawer after four bags: posed");
        Check(ShouldPoseShared(false, 16, 60, 39, shared), "the last shared slot is used");
        Check(!ShouldPoseShared(false, 16, 60, 40, shared), "objects and weapons never pass the shared slots");
        Check(!ShouldPoseShared(true, 16, 0, 0, shared), "a real weapon goes through ShouldPoseWeapon");
        Check(!ShouldPoseShared(false, 1, 0, 0, shared), "a rigid object has nothing to pose");
        // The bodies keep every place: the ped class is never given up.
        Check(cap::kGtaDefaultPedCapacity + cap::kMaxVehicleCapacity + shared <= cap::kLiveTargetSlots,
            "bodies, weapons and objects fit the slots");
        const std::uint32_t legacy =
            WeaponPlaces(cap::kLiveTargetSlots, cap::LiveTargets(cap::kDefaultPedCapacity, cap::kLegacyVehicleCapacity));
        Check(legacy == cap::kLiveTargetSlots - 64, "without the GTA model: every slot past the 64 bodies");
    }
}

int RunLootTests()
{
    g_lootFailures = 0;
    TestTakenDressing();
    TestDressingRemoval();
    TestNearWatch();
    TestSharedPlaces();
    if (g_lootFailures == 0)
        std::printf("loot tests passed\n");
    return g_lootFailures;
}
