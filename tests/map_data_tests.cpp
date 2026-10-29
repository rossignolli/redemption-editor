// The map's objects by their creator, not by the most frequent owner. The
// owners 14, 17, 24 and 25 are map data (the map data instance path writes 25
// and stores the slot and instance index; the map data definition path
// writes 14); every other creator's object is scenery. Map data inside the
// sphere of a clip IPL that is on is kept as map dressing with that IPL and
// built at playback only when the map does not have it (e.g. a gang camp IPL
// left off during a mission).
#include "object_check.h"

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

namespace
{
    using namespace replay::objcheck;

    int g_mapDataFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_mapDataFailures;
    }

    void TestMapDataOwners()
    {
        Check(IsMapDataOwner(kOwnerMapInstance) && IsMapDataOwner(kOwnerMapDefinition) && IsMapDataOwner(kOwnerInterior) &&
                  IsMapDataOwner(kOwnerLent),
            "map data: 25 (instance), 14 (definition), 17 (interior), 24 (lent)");
        Check(!IsMapDataOwner(0) && !IsMapDataOwner(1) && !IsMapDataOwner(4) && !IsMapDataOwner(kOwnerMission) &&
                  !IsMapDataOwner(kOwnerPropSet) && !IsMapDataOwner(kOwnerTempA) && !IsMapDataOwner(kOwnerUnknown) &&
                  !IsMapDataOwner(31) && !IsMapDataOwner(40),
            "map data: no other owner, an unread one neither");
        Check(kWorldOwnerMask == kMapDataOwnerMask, "map data: the older mask name is the same set");
    }

    void TestSceneryRules()
    {
        // A snapshot of 153 objects, all owner 25: map data, refused because
        // of who made them.
        Check(SceneryRuleOf(false, 25, false) == SceneryRule::MapData, "rule: owner 25 is map data");
        Check(SceneryRuleOf(true, 25, false) == SceneryRule::Script, "rule: a mission entity is the script's whoever owns it");
        Check(SceneryRuleOf(false, kOwnerMission, false) == SceneryRule::Script &&
                  SceneryRuleOf(false, kOwnerPropSet, false) == SceneryRule::Script,
            "rule: CREATE_OBJECT's 5 and a prop set's 23");
        Check(SceneryRuleOf(false, kOwnerTempA, false) == SceneryRule::Temp && SceneryRuleOf(false, kOwnerTempB, false) == SceneryRule::Temp,
            "rule: the temporary owners");
        Check(SceneryRuleOf(false, 25, true) == SceneryRule::Pickup, "rule: a pickup whoever owns it");
        Check(SceneryRuleOf(false, 4, false) == SceneryRule::Other && SceneryRuleOf(false, 0, false) == SceneryRule::Other &&
                  SceneryRuleOf(false, 18, false) == SceneryRule::Other,
            "rule: any other creator (persistence 4, 0, 18) is other");
        Check(SceneryRuleOf(false, kOwnerUnknown, false) == SceneryRule::Unknown, "rule: an unread owner is unknown");
        // Scenery follows the rule: Other is an entry (every non-map object
        // gets a create); map data and unknown are not.
        Check(SceneryCandidate(false, 4, false, false) && SceneryCandidate(false, 18, false, false),
            "scenery: other creators are entries");
        Check(!SceneryCandidate(false, 14, false, false) && !SceneryCandidate(false, 25, false, false) &&
                  !SceneryCandidate(false, kOwnerUnknown, false, false),
            "scenery: map data and unread owners are no entries");
        Check(IsLootEntry(false, 25) && !IsLootEntry(false, 4), "loot: a map object's take only");
        Check(std::string_view(SceneryRuleText(SceneryRule::MapDressing)) == "map_dressing" &&
                  std::string_view(SceneryRuleText(SceneryRule::MapData)) == "map_data",
            "rule names for the log");
    }

    void TestDressingIpl()
    {
        // The gang camp (a small sphere) inside a larger region's.
        std::vector<DressingSphere> spheres;
        DressingSphere region;
        region.hash = 0x11111111u;
        region.center[0] = 700.0f;
        region.center[1] = -1200.0f;
        region.radius = 400.0f;
        DressingSphere camp;
        camp.hash = 0xF67D5695u;
        camp.center[0] = 680.0f;
        camp.center[1] = -1255.0f;
        camp.center[2] = 43.0f;
        camp.radius = 40.0f;
        spheres.push_back(region);
        spheres.push_back(camp);
        const float tent[3]{677.75f, -1254.76f, 43.04f};
        Check(DressingIplAt(tent, spheres) == 0xF67D5695u, "dressing: the smallest sphere holding it (the camp's IPL)");
        const float field[3]{900.0f, -1100.0f, 50.0f};
        Check(DressingIplAt(field, spheres) == 0x11111111u, "dressing: only the region holds it");
        const float farAway[3]{3000.0f, 0.0f, 0.0f};
        Check(DressingIplAt(farAway, spheres) == 0, "dressing: no sphere, the always-on map's (IPL 0)");
        DressingSphere empty;
        empty.hash = 0x22222222u;
        empty.center[0] = tent[0];
        empty.center[1] = tent[1];
        empty.center[2] = tent[2];
        empty.radius = 0.0f;
        Check(DressingIplAt(tent, {empty}) == 0, "dressing: a sphere without a radius never holds");
        Check(MapDressingGone(false) && !MapDressingGone(true), "dressing: gone only with its IPL off");
    }

    void TestDressingVerdict()
    {
        // The camp IPL off (a mission ran, the world state only counted):
        // built at once.

        Check(MapDressingVerdict(false, false, false, false, 0) == MapDressing::Build, "verdict: IPL off, built at once");
        // The same clip outside a mission (the IPL applied): the map has it.
        Check(MapDressingVerdict(false, false, true, true, 100) == MapDressing::Live, "verdict: original there, left to the map");
        Check(MapDressingVerdict(false, false, true, false, kMapDressingWaitMs - 1) == MapDressing::Wait,
            "verdict: IPL on, looked for a while");
        Check(MapDressingVerdict(false, false, true, false, kMapDressingWaitMs) == MapDressing::Build,
            "verdict: IPL on and still missing (a shop's shelf dressing): built");
        Check(MapDressingVerdict(false, true, true, true, 0) == MapDressing::Build,
            "verdict: its visibility changed while recorded (a take): played as scenery");
        // Taken by a prop (1212, 1213: the drawer and the shelf item were map
        // dressing): played as scenery, its original hidden, never left drawn
        // next to the prop.
        Check(MapDressingVerdict(true, false, true, true, 100) == MapDressing::Build,
            "verdict: a prop took it over, the original there: built, the original hidden");
        Check(MapDressingVerdict(true, true, false, false, 0) == MapDressing::Build, "verdict: a prop took it over");
        Check(std::string_view(MapDressingText(MapDressing::Live)) == "live", "verdict names");
    }
}

int RunMapDataTests()
{
    g_mapDataFailures = 0;
    TestMapDataOwners();
    TestSceneryRules();
    TestDressingIpl();
    TestDressingVerdict();
    if (g_mapDataFailures == 0)
        std::printf("map data tests passed\n");
    return g_mapDataFailures;
}
