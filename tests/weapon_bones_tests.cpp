// The weapons' own bones: recorded for every weapon in a hand within the free
// capture slots, and the drawn check of the playback.
#include "capture_capacity.h"
#include "fx_check_core.h"
#include "object_pose.h"
#include "weapon_bones_core.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_weaponBonesFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_weaponBonesFailures;
    }

    replay::pose::Quat AxisZ(float rad)
    {
        replay::pose::Quat q;
        q.z = std::sin(rad * 0.5f);
        q.w = std::cos(rad * 0.5f);
        return q;
    }

    void TestPlaces()
    {
        namespace op = replay::object_pose;
        namespace cap = replay::capacity;
        // The GTA model (148 peds + 79 vehicles): still 100 slots left for the
        // weapons; without it (56 + 8) at least the 192 of before.
        Check(op::WeaponPlaces(cap::kLiveTargetSlots,
                  cap::LiveTargets(cap::kGtaDefaultPedCapacity, cap::VehicleCapacityFor(true))) == 100,
            "100 weapon places with both whole pools");
        Check(op::WeaponPlaces(cap::kLiveTargetSlots,
                  cap::LiveTargets(cap::kDefaultPedCapacity, cap::VehicleCapacityFor(false))) >= 192,
            "at least 192 weapon places at 56 peds and 8 vehicles");
        Check(op::WeaponPlaces(10, 20) == 0, "no slot left: none");
        Check(op::ShouldPoseWeapon(true, 48, 21, 192), "the 22nd weapon of 1156 is posed");
        Check(!op::ShouldPoseWeapon(true, 48, 192, 192), "the slots are the only limit");
        // Bodies: the weapons stop at their count.
        Check(op::WatchWeaponBones(true, 0x169F59F7u, 0, false, false), "any weapon in a hand is watched, no cap");
    }

    void TestDiff()
    {
        namespace wb = replay::weaponbones;
        std::vector<replay::pose::BoneQts> a(30), b(30);
        Check(wb::DiffLocals(a.data(), b.data(), a.size()).moved == 0, "same pose: nothing moved");
        b[8].rotation = AxisZ(3.14159f); // a revolver cylinder (bone 8, turned 180 degrees)
        wb::PoseDiff d = wb::DiffLocals(a.data(), b.data(), a.size());
        Check(d.moved == 1 && d.worstBone == 8 && d.maxRad > 3.1f, "the cylinder turn is seen");
        b[8].rotation = AxisZ(0.01f); // under a degree
        Check(wb::DiffLocals(a.data(), b.data(), a.size()).moved == 0, "under 1 degree is the same");
        b[0].rotation = AxisZ(1.0f); // the root follows the attach
        Check(wb::DiffLocals(a.data(), b.data(), a.size()).moved == 0, "bone 0 left out");
        b[40 % 30].translation[0] = 0.02f;
        Check(wb::DiffLocals(a.data(), b.data(), a.size()).moved == 1, "2 cm of the lever is seen");
        b[3].translation[1] = std::nanf("");
        Check(wb::DiffLocals(a.data(), b.data(), a.size()).moved == 1, "a NaN read is not a verdict");
    }

    void TestVerdict()
    {
        namespace wb = replay::weaponbones;
        wb::PoseDiff same;
        wb::PoseDiff other;
        other.moved = 3;
        other.maxRad = 1.0f;
        other.worstBone = 8;
        // The player's repeater: 3248 of 3250 writes overwritten.
        wb::PlayCheck lost;
        for (int n = 0; n < 40; ++n)
            wb::NoteCheck(lost, n % 2 == 0, other);
        Check(wb::Verdict(lost) == wb::PlayVerdict::Overwritten, "overwritten while the track moves");
        Check(lost.worstBone == 8, "the worst bone kept");
        wb::PlayCheck shown;
        for (int n = 0; n < 40; ++n)
            wb::NoteCheck(shown, n % 2 == 0, same);
        Check(wb::Verdict(shown) == wb::PlayVerdict::Shown, "the recorded motion drawn");
        wb::PlayCheck still;
        for (int n = 0; n < 40; ++n)
            wb::NoteCheck(still, false, other);
        Check(wb::Verdict(still) == wb::PlayVerdict::None, "no motion to show: no verdict (the engine may pose a still weapon)");
        wb::PlayCheck mixed;
        for (int n = 0; n < 40; ++n)
            wb::NoteCheck(mixed, true, n % 4 == 0 ? other : same);
        Check(wb::Verdict(mixed) == wb::PlayVerdict::Mixed, "a quarter lost: mixed");
        Check(std::string(wb::VerdictName(wb::PlayVerdict::Overwritten)) == "overwritten", "names");

        replay::fxcheck::Tally tally;
        tally.Reset();
        tally.NoteWeaponBones(12, 12); // all armed, weapon_bones_static silent
        tally.NoteWeaponBonesDrawn(1, 1, 3);
        bool found = false;
        for (const replay::fxcheck::Flag& flag : tally.Flags())
            found = found || (flag.kind == "weapon_bones_overwritten" && flag.value == 2.0);
        Check(found, "weapon_bones_overwritten counts overwritten and mixed");
        tally.ResetPlay();
        Check(tally.Flags().empty(), "reset with the play");
    }

    // The cylinder turned in the record and still in the drawing, with the
    // whole-pose verdict "shown".
    void TestFrozen()
    {
        namespace wb = replay::weaponbones;
        Check(wb::FrozenBone(18, 0), "turned 18 ticks in the record, never drawn: frozen");
        Check(!wb::FrozenBone(18, 17), "drawn as recorded: not frozen");
        Check(!wb::FrozenBone(2, 0), "two ticks are not enough");
        Check(wb::FrozenBone(20, 4) && !wb::FrozenBone(20, 5), "under a quarter drawn is frozen");
        replay::fxcheck::Tally tally;
        tally.Reset();
        tally.NoteWeaponBones(9, 9);
        tally.NoteWeaponBonesDrawn(0, 0, 8);
        tally.NoteWeaponBonesFrozen(1);
        bool found = false;
        for (const replay::fxcheck::Flag& flag : tally.Flags())
            found = found || (flag.kind == "weapon_bones_frozen" && flag.value == 1.0);
        Check(found && tally.Flags().size() == 1, "weapon_bones_frozen with every weapon shown");
        tally.ResetPlay();
        Check(tally.Flags().empty(), "frozen reset with the play");
    }
}

int RunWeaponBonesTests()
{
    TestPlaces();
    TestDiff();
    TestVerdict();
    TestFrozen();
    if (g_weaponBonesFailures == 0)
        std::printf("weapon bones tests: ok\n");
    return g_weaponBonesFailures;
}
