// The rules of the world objects as state packets (object_check.h), the
// weapon object's weapon, the scenery's life and window, and the QA line of
// the verdicts.
#include "object_check.h"
#include "qa_health.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    using namespace replay;
    using namespace replay::objcheck;

    int g_objectFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_objectFailures;
    }

    entity::CreatePacket Create(std::uint16_t slot, entity::EntityKind kind, std::uint32_t model, std::uint32_t weapon,
        entity::PropOrigin origin = entity::PropOrigin::DroppedWeapon)
    {
        entity::CreatePacket packet;
        packet.id = entity::ReplayId::FromPacked((static_cast<std::uint32_t>(slot) << 16) | 1u);
        packet.kind = kind;
        packet.modelHash = model;
        packet.weaponHash = weapon;
        packet.propOrigin = origin;
        return packet;
    }

    void TestLayout()
    {
        // Build 1491.50: HAS_WEAPON_GOT_WEAPON_COMPONENT's core reads [object+0x448];
        // the give core passes [weapon+0x68] as the weapon hash;
        // DeleteAllMissionObjects tests (+0xB4 & 0x7C000000) == 0x14000000 (owner 5);
        // DeleteAllTempObjects owner 2 or 3.
        Check(kObjectWeaponOffset == 0x448 && kWeaponHashOffset == 0x68, "layout: weapon object offsets");
        Check(OwnerOf(0x14000000u) == kOwnerMission, "layout: 0x14000000 is owner 5 (mission)");
        Check(OwnerOf(0x08000000u) == kOwnerTempA && OwnerOf(0x0C000000u) == kOwnerTempB, "layout: temp owners 2 and 3");
        Check(OwnerOf(0x83FFFFFFu) == 0, "layout: other bits are not the owner");
    }

    void TestWeapon()
    {
        const std::uint32_t knifeModel = 0xF654A1D1u; // w_melee_knife02
        const std::uint32_t knife = 0xDB21AC8Cu;      // WEAPON_MELEE_KNIFE
        Check(ResolveObjectWeapon(knife, knifeModel, knifeModel) == knife, "weapon: the read weapon whose model is the object's");
        Check(ResolveObjectWeapon(knife, 0x12345678u, knifeModel) == 0, "weapon: another model is refused");
        Check(ResolveObjectWeapon(0, 0, knifeModel) == 0 && ResolveObjectWeapon(knife, 0, knifeModel) == 0,
            "weapon: nothing read or an unknown weapon type is refused");

        // The knife in the hand of a fillet player and the one left on the table, bare
        // models; Arthur's own knife carries the weapon.
        std::vector<entity::CreatePacket> created;
        created.push_back(Create(46, entity::EntityKind::Weapon, knifeModel, knife));
        created.push_back(Create(103, entity::EntityKind::Weapon, knifeModel, 0));
        created.push_back(Create(101, entity::EntityKind::Prop, knifeModel, 0, entity::PropOrigin::DroppedWeapon));
        created.push_back(Create(13, entity::EntityKind::Prop, knifeModel, 0, entity::PropOrigin::DroppedHat));
        created.push_back(Create(50, entity::EntityKind::Prop, 0x5F5F56E4u, 0, entity::PropOrigin::WorldObject));
        const auto models = WeaponModels(created);
        Check(models.size() == 1 && models.at(knifeModel) == knife, "weapon: the clip's weapon models");
        Check(!WeaponPlain(created[0], models), "weapon: a weapon with its weapon is not plain");
        Check(WeaponPlain(created[1], models), "weapon: the knife in the hand without its weapon is plain");
        Check(WeaponPlain(created[2], models), "weapon: the knife in the table without its weapon is plain");
        Check(!WeaponPlain(created[3], models), "weapon: a hat is rebuilt another way");
        Check(!WeaponPlain(created[4], models), "weapon: a model no weapon has is not plain");
        Check(std::string(OriginText(entity::PropOrigin::WorldObject)) == "world_object", "weapon: origin names");
    }

    void TestScenery()
    {
        Check(IsSceneryOwner(true, 0) && IsSceneryOwner(false, kOwnerMission) && IsSceneryOwner(false, kOwnerTempA) &&
                  IsSceneryOwner(false, kOwnerTempB),
            "scenery: mission entities and the script and temporary owners");
        // Any creator that is not map data is scenery; the map data owners (14, 17,
        // 24, 25) and an unread owner are not.
        Check(IsSceneryOwner(false, 0) && IsSceneryOwner(false, 1) && IsSceneryOwner(false, 4) &&
                  !IsSceneryOwner(false, kOwnerUnknown),
            "scenery: any non map creator is scenery, an unread owner is not");
        Check(!IsSceneryOwner(false, 14) && !IsSceneryOwner(false, 17) && !IsSceneryOwner(false, 24) &&
                  !IsSceneryOwner(false, 25) && IsSceneryOwner(true, 25),
            "scenery: the map data owners are not scenery unless a mission entity");

        // Life: from appear to gone, before the takeover, 500 ms of grace.
        Check(!SceneryShown(1000, kNeverGone, false, 0, false, 999) && SceneryShown(1000, kNeverGone, false, 0, false, 1000),
            "scenery: not before it appeared");
        Check(SceneryShown(0, 5000, false, 0, false, 4999) && !SceneryShown(0, 5000, false, 0, false, 5000),
            "scenery: not once it left");
        Check(SceneryShown(0, kNeverGone, true, 3000, true, 2999) && !SceneryShown(0, kNeverGone, true, 3000, true, 3000),
            "scenery: the prop takes over when ready");
        Check(SceneryShown(0, kNeverGone, true, 3000, false, 3400) && !SceneryShown(0, kNeverGone, true, 3000, false, 3500),
            "scenery: the stand-in stays 500 ms for a prop not built");

        // The window [10000, 20000).
        std::uint32_t appear = 0, gone = kNeverGone;
        Check(CutLife(appear, gone, 10000, 20000) && appear == 0 && gone == kNeverGone, "scenery: alive across the window");
        appear = 12000;
        gone = 15000;
        Check(CutLife(appear, gone, 10000, 20000) && appear == 2000 && gone == 5000, "scenery: moved into the window");
        appear = 0;
        gone = 9000;
        Check(!CutLife(appear, gone, 10000, 20000), "scenery: gone before the window is dropped");
        appear = 20000;
        gone = kNeverGone;
        Check(!CutLife(appear, gone, 10000, 20000), "scenery: appearing after the window is dropped");
        appear = 5000;
        gone = 25000;
        Check(CutLife(appear, gone, 10000, 20000) && appear == 0 && gone == kNeverGone, "scenery: leaving after the window stays");

        Check(SceneryReady(0, 0, 3000) && !SceneryReady(3, 1000, 3000) && SceneryReady(3, 3000, 3000),
            "scenery: the start waits for the pending stand-ins up to the cap");
        const float a[3]{1.0f, 2.0f, 3.0f};
        const float b[3]{1.3f, 2.3f, 3.0f};
        Check(Near(a, b, kOriginalRadiusM) && !Near(a, b, 0.3f), "scenery: the original's radius");
    }

    void TestVerdictLine()
    {
        Check(std::string(VerdictText(Verdict::Duplicate)) == "duplicate" &&
                  std::string(VerdictText(Verdict::FragUnplayed)) == "frag_unplayed" &&
                  std::string(VerdictText(Verdict::WeaponPlain)) == "weapon_plain",
            "verdicts: names");
        qa::Anomaly anomaly;
        Check(qa::ClassifyLine("POSE_V2_OBJECT_CHECK,id=74:974,verdict=frag_unplayed,model=0xe862e5ab,why=no_puppet,playhead_ms=7019",
                  0, anomaly) &&
                  anomaly.kind == qa::Kind::ObjectWrong && anomaly.entity == "74:974" && anomaly.playheadMs == 7019 &&
                  anomaly.detail.find("verdict=frag_unplayed") != std::string::npos,
            "verdicts: the QA ring takes the line");
        Check(qa::ClassifyLine("POSE_V2_OBJECT_CHECK,id=scenery,verdict=duplicate,model=0x490cc7c7,phase=play,why=hidden_late", 4200,
                  anomaly) &&
                  anomaly.playheadMs == 4200,
            "verdicts: a line without its own playhead takes the current one");
        Check(std::string(qa::KindText(qa::Kind::ObjectWrong)) == "object_wrong", "verdicts: kind name");
    }
}

int RunObjectCheckTests()
{
    g_objectFailures = 0;
    TestLayout();
    TestWeapon();
    TestScenery();
    TestVerdictLine();
    if (g_objectFailures == 0)
        std::printf("object check tests passed\n");
    return g_objectFailures;
}
