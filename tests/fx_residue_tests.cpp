// The pure parts of the particle and weapon residue checks (P1..P5, A,
// B, C, D), with numbers taken from recorded logs.
#include "fx_check_core.h"
#include "fx_motion_rule.h"
#include "object_pose.h"
#include "weapon_fx_intent_core.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_residueFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_residueFailures;
    }

    using replay::fxcheck::Flag;
    using replay::fxcheck::Tally;

    bool HasFlag(const std::vector<Flag>& flags, const char* kind, std::uint32_t hash)
    {
        for (const Flag& flag : flags)
            if (flag.kind == kind && flag.hash == hash)
                return true;
        return false;
    }

    double FlagValue(const std::vector<Flag>& flags, const char* kind, std::uint32_t hash)
    {
        for (const Flag& flag : flags)
            if (flag.kind == kind && flag.hash == hash)
                return flag.value;
        return -1.0;
    }

    // P1: the rate of recreate_loop on the wall clock: 79 recreations of a
    // chimney smoke while paused at 329 ms, one per tick (16 ms).
    void TestRecreateWallRate()
    {
        Tally paused;
        paused.Reset();
        for (std::uint32_t n = 0; n < 79; ++n)
            paused.NoteRecreate(0x5cdc744c, 2, 4, 329, 100000 + n * 16);
        const std::vector<Flag> flags = paused.Flags();
        Check(HasFlag(flags, "recreate_loop", 0x5cdc744c), "79 recreations in 1.3 s of a pause are a loop");
        const double rate = FlagValue(flags, "recreate_loop", 0x5cdc744c);
        Check(rate > 55.0 && rate < 65.0, "the paused loop's rate is per wall second (about 60, not 79 per playhead second)");
        // Four recreations a playback minute apart on the wall: not a loop
        // even when the playhead says they were close (a jump back).
        Tally slow;
        slow.Reset();
        for (std::uint32_t n = 0; n < 4; ++n)
            slow.NoteRecreate(0x4dff15fc, 0, 2, 1000 + n * 10, 200000 + n * 15000);
        Check(!HasFlag(slow.Flags(), "recreate_loop", 0x4dff15fc), "four recreations over 45 s of wall are not a loop");
        // Without a wall clock (older callers): the playhead.
        Tally legacy;
        legacy.Reset();
        for (std::uint32_t n = 0; n < 24; ++n)
            legacy.NoteRecreate(0x1, 0, 1, 100 + n * 417);
        Check(HasFlag(legacy.Flags(), "recreate_loop", 0x1), "the playhead rate stays without a wall clock");
    }

    // P5: the derived basis marks only stretches of two ticks or more, GapAt
    // names the stretch, Entered is final.
    void TestGapsP5()
    {
        namespace fxm = replay::fxmotion;
        std::vector<std::uint32_t> times;
        std::uint32_t t = 1000;
        for (int n = 0; n < 20; ++n, t += 16)
            times.push_back(t);
        t += 16; // one tick without a frame (32 ms step)
        for (int n = 0; n < 20; ++n, t += 16)
            times.push_back(t);
        const std::uint32_t twoFrom = times.back();
        t += 32; // two ticks without a frame (48 ms step)
        for (int n = 0; n < 10; ++n, t += 16)
            times.push_back(t);
        Check(fxm::DeriveCallGaps(times).size() == 2, "one missing tick is a stretch with the old rule");
        const std::vector<replay::entity::RegisteredFxGap> two = fxm::DeriveCallGaps(times, 2);
        Check(two.size() == 1 && two[0].fromMs == twoFrom, "with two ticks only the 48 ms step is a stretch");
        const std::vector<replay::entity::RegisteredFxGap> gaps = {{1000, 1066}, {2000, 2100}};
        Check(fxm::GapAt(gaps, 1030, 1014) == 0, "inside the first stretch");
        Check(fxm::GapAt(gaps, 2050, 2034) == 1, "inside the second stretch");
        Check(fxm::GapAt(gaps, 2200, 1990) == 1, "a step over the second stretch names it");
        Check(fxm::GapAt(gaps, 1500, 1480) == -1, "between the stretches");
        Check(fxm::MergeGapSeen(fxm::GapSeen::Entered, fxm::GapSeen::Hold) == fxm::GapSeen::Entered, "entered is final");
        Check(fxm::MergeGapSeen(fxm::GapSeen::NoEntry, fxm::GapSeen::Hold) == fxm::GapSeen::Hold, "the last reason wins");
        Check(std::string(fxm::GapSeenName(fxm::GapSeen::OwnerAbsent)) == "owner_absent", "reason names");
    }

    // B: a real weapon in a hand is watched and posed in places of its own.
    void TestWeaponBonesB()
    {
        namespace op = replay::object_pose;
        Check(op::InHandPoint(0) && op::InHandPoint(1) && !op::InHandPoint(2) && !op::InHandPoint(-1),
            "points 0 and 1 are the hands (the held weapon is point 0)");
        Check(op::WatchWeaponBones(true, 0x63CA782Au, 0, false, false), "the repeater in the hand is watched");
        Check(!op::WatchWeaponBones(false, 0x63CA782Au, 0, false, false), "WeaponBonePose=0: none");
        Check(!op::WatchWeaponBones(true, 0, 0, false, false), "an object without a weapon (the pelt) keeps its own path");
        Check(!op::WatchWeaponBones(true, 0x63CA782Au, 2, false, false), "a holstered weapon is not watched");
        Check(!op::WatchWeaponBones(true, 0x63CA782Au, 0, true, false), "a posed weapon is not watched again");
        // The places are the free capture slots, no cap of 8
        // (weapon_bones_tests.cpp).
        Check(op::ShouldPoseWeapon(true, 12, 3, 4) && !op::ShouldPoseWeapon(true, 12, 4, 4), "the places given are the limit");
        Check(!op::ShouldPoseWeapon(true, 1, 0, 4), "a rigid weapon (one bone) has nothing to pose");
        Check(op::ShouldPoseWeapon(true, 12, 8, 50), "no cap of 8");
        // The places of the bodies and of the props do not change: the old rule
        // still refuses a real weapon (it goes through ShouldPoseWeapon only).
        Check(!op::ShouldPose(true, 12, 10, 0, 4, 56, 10, 64), "ShouldPose still refuses a real weapon");
    }

    // A and D: the per-shot audit and the two new findings.
    void TestShotAuditD()
    {
        namespace wf = replay::weaponfx;
        // The gunfight: the player's 4 shots with a flash, 3 NPC shots
        // without, one NPC shot whose flash two shots claimed (shared, an
        // older clip), a shotgun's second pellet row (not a shot).
        std::vector<wf::ShotRow> shots;
        for (std::uint32_t n = 0; n < 4; ++n)
        {
            wf::ShotRow row;
            row.timeMs = 1000 + n * 500;
            row.shooter = 0x00010001u;
            row.player = true;
            row.muzzleEvent = true;
            row.inView = 1;
            shots.push_back(row);
        }
        for (std::uint32_t n = 0; n < 3; ++n)
        {
            wf::ShotRow row;
            row.timeMs = 10471 + n * 17;
            row.shooter = 0x00050005u + n;
            row.inView = 0;
            shots.push_back(row);
        }
        wf::ShotRow shared;
        shared.timeMs = 10488;
        shared.shooter = 0x00090009u;
        shared.hasLegacyMuzzle = true;
        shared.muzzleShared = true;
        shots.push_back(shared);
        wf::ShotRow pellet = shots[0];
        shots.push_back(pellet);
        wf::Track none;
        const wf::ShotAudit legacy = wf::AuditShots(shots, none);
        Check(legacy.shots == 8, "a pellet of the same shot at the same ms is not another shot");
        Check(legacy.withMuzzle == 4 && legacy.noMuzzle == 4 && legacy.noMuzzleNpc == 4,
            "the three NPC shots and the shared one have no flash of their own");
        Check(legacy.noMuzzleOutOfView == 3 && legacy.culled == 0 && legacy.noRequest == 0, "without requests: out of view only");
        wf::Track requests;
        wf::Intent ask;
        ask.kind = static_cast<std::uint8_t>(wf::Kind::Muzzle);
        ask.timeMs = 10471;
        ask.shooter = 0x00050005u;
        requests.Add(ask);
        ask.timeMs = 10505;
        ask.shooter = 0x00050007u;
        requests.Add(ask);
        const wf::ShotAudit withRequests = wf::AuditShots(shots, requests);
        Check(withRequests.culled == 2 && withRequests.noRequest == 2, "two shots asked and culled, two without a request");

        const float cam[3] = {0.0f, 0.0f, 0.0f};
        const float ahead[3] = {0.0f, 10.0f, 0.0f};
        const float side[3] = {10.0f, 0.0f, 0.0f};
        Check(wf::InView(cam, 0.0f, 0.0f, 50.0f, ahead), "a point ahead (yaw 0 looks along +y) is in view");
        Check(!wf::InView(cam, 0.0f, 0.0f, 50.0f, side), "a point 90 degrees to the side is not");
        Check(wf::InView(cam, 0.0f, -90.0f, 50.0f, side), "yaw -90 looks along +x");

        Tally tally;
        tally.Reset();
        tally.NoteShots(4, 2);
        tally.NoteWeaponBones(3, 1);
        const std::vector<Flag> flags = tally.Flags();
        Check(HasFlag(flags, "shot_no_fx", 0) && FlagValue(flags, "shot_no_fx", 0) == 2.0,
            "shot_no_fx counts the shots without a request (the culled ones are called again)");
        Check(HasFlag(flags, "weapon_bones_static", 0) && FlagValue(flags, "weapon_bones_static", 0) == 2.0,
            "weapon_bones_static: two weapon poses reached and never armed");
        tally.ResetPlay();
        Check(tally.Flags().empty(), "both reset with the play");
        tally.NoteShots(18, 18); // a gunfight: 18 NPC shots culled live, all requested
        Check(!HasFlag(tally.Flags(), "shot_no_fx", 0), "every shot without a muzzle has a request: no shot_no_fx");
        tally.ResetPlay();
    }
}

namespace
{
    // The requests played back, the recorded effects they replace and the
    // call's arguments.
    void TestWeaponFxPlay()
    {
        namespace wf = replay::weaponfx;
        wf::Intent muzzle;
        muzzle.kind = static_cast<std::uint8_t>(wf::Kind::Muzzle);
        muzzle.timeMs = 10471;
        muzzle.shooter = 0x00050005u;
        muzzle.weapon = 0x00090002u;
        muzzle.bone = 3;
        muzzle.argA = 0;
        muzzle.fxHash = 0x1234ABCDu;
        muzzle.fxHashAlt = 0x0BADF00Du;
        muzzle.range = 60.0f;
        muzzle.vec[0] = 0.25f;
        wf::Intent shell = muzzle;
        shell.kind = static_cast<std::uint8_t>(wf::Kind::Shell);
        shell.timeMs = 10480;
        shell.bone = 7;
        shell.argA = 5;
        shell.fxHash = shell.fxHashAlt = 0;
        shell.range = 0.0f;
        Check(wf::Playable(muzzle) && wf::Playable(shell), "a request with weapon and bone is playable");
        wf::Intent bad = muzzle;
        bad.argA = 2;
        Check(!wf::Playable(bad), "a muzzle's last dword indexes CWeapon+0x528: only 0 or 1");
        Check(wf::Playable(shell), "a shell's dword is a registry slot, any value");
        bad = muzzle;
        bad.weapon = wf::kNoId;
        Check(!wf::Playable(bad), "no weapon object recorded: nothing to call with");
        bad = muzzle;
        bad.bone = -1;
        Check(!wf::Playable(bad), "bone -1: the core leaves at once, not played");

        wf::Track track;
        Check(track.Add(muzzle) && track.Add(shell), "requests: add");
        const std::uint32_t site = wf::kMuzzleEffectSite;
        Check(wf::CoversMuzzleEvent(track, {site, 10480, muzzle.weapon}), "the muzzle effect on the weapon 9 ms later is replaced");
        Check(wf::CoversMuzzleEvent(track, {site, 10420, muzzle.shooter}), "on the shooter within 60 ms too");
        Check(!wf::CoversMuzzleEvent(track, {site, 10600, muzzle.weapon}), "129 ms later it is another shot's");
        Check(!wf::CoversMuzzleEvent(track, {site, 10480, 0x00070001u}), "another entity's effect stays");
        Check(!wf::CoversMuzzleEvent(track, {0x144D33A, 10480, muzzle.weapon}), "an impact is not a muzzle effect");
        wf::Track unplayable;
        bad = muzzle;
        bad.bone = -1;
        unplayable.Add(bad);
        Check(!wf::CoversMuzzleEvent(unplayable, {site, 10471, muzzle.weapon}), "a request not played drops nothing");
        Check(wf::CoversShellTrack(track, {wf::kShellRegisteredSite, 9000, 12000, muzzle.weapon}),
            "the shell track alive over the request is replaced");
        Check(wf::CoversShellTrack(track, {wf::kShellRegisteredSite, 10530, 12000, muzzle.weapon}),
            "a track that starts 50 ms after the request is its");
        Check(!wf::CoversShellTrack(track, {wf::kShellRegisteredSite, 10600, 12000, muzzle.weapon}),
            "a track that starts 120 ms after is not");
        Check(!wf::CoversShellTrack(track, {wf::kShellRegisteredSite, 1000, 10400, muzzle.weapon}),
            "a track that ended 80 ms before is not");
        Check(!wf::CoversShellTrack(track, {0x146798C, 9000, 12000, muzzle.weapon}), "another site is not the shell");

        wf::CallInput in;
        in.shooter = 0x7FF612340000ull;
        in.object = 0x7FF612350000ull;
        in.weapon = 0x7FF612360000ull;
        in.weaponInfo = 0x7FF612370000ull;
        in.info = 0x7FF612380000ull;
        wf::CallPlan plan = wf::PlanCall(muzzle, in);
        Check(plan.ok && plan.shooter == in.shooter && plan.weapon == in.weapon && plan.object == in.object &&
                  plan.info == in.info && plan.bone == 3 && plan.argA == 0 && plan.vec[0] == 0.25f,
            "muzzle: shooter, CWeapon, object, bone, info, vector and dword in the wrapper's order");
        plan = wf::PlanCall(shell, in);
        Check(plan.ok && plan.info == 0 && plan.bone == 7 && plan.argA == 5, "shell: no info, its own bone and slot");
        wf::CallInput none = in;
        none.shooter = 0;
        Check(wf::PlanCall(muzzle, none).ok, "no shooter puppet: both cores test it, the call goes with null");
        none = in;
        none.object = 0;
        Check(std::string(wf::PlanCall(muzzle, none).why) == "no_weapon_puppet", "no weapon puppet: not called");
        none = in;
        none.weapon = 0;
        Check(std::string(wf::PlanCall(shell, none).why) == "no_cweapon", "a weapon object without CWeapon: not called");
        none = in;
        none.weaponInfo = 0;
        Check(std::string(wf::PlanCall(shell, none).why) == "no_weapon_info", "a CWeapon without info: not called");
        none = in;
        none.info = 0;
        Check(std::string(wf::PlanCall(muzzle, none).why) == "no_info" && wf::PlanCall(shell, none).ok,
            "the muzzle needs its info entry, the shell does not");
        none = in;
        none.shooter = 0x1234;
        Check(std::string(wf::PlanCall(muzzle, none).why) == "bad_shooter", "an implausible shooter is refused");

        std::uint8_t head[12] = {};
        std::memcpy(head, &muzzle.fxHash, 4);
        std::memcpy(head + 4, &muzzle.fxHashAlt, 4);
        std::memcpy(head + 8, &muzzle.range, 4);
        Check(wf::InfoMatches(head, muzzle), "the info entry with the recorded hashes and range");
        const float other = 61.0f;
        std::memcpy(head + 8, &other, 4);
        Check(!wf::InfoMatches(head, muzzle), "another range is another entry");

        // Planes: x >= 0, y >= 0, z >= 0, and a plane always true, twice.
        float planes[8][4] = {};
        for (int set = 0; set < 2; ++set)
        {
            planes[set * 4 + 0][0] = 1.0f;
            planes[set * 4 + 1][1] = 1.0f;
            planes[set * 4 + 2][2] = 1.0f;
            planes[set * 4 + 3][3] = 1.0f;
        }
        const float inside[3] = {1.0f, 2.0f, 3.0f};
        const float edge[3] = {-0.4f, 2.0f, 3.0f};
        const float outside[3] = {-0.6f, 2.0f, 3.0f};
        Check(wf::EngineInView(planes, inside), "a point inside every plane is in view");
        Check(wf::EngineInView(planes, edge), "within the 0.5 radius of a plane is still in view");
        Check(!wf::EngineInView(planes, outside), "past the radius it is out");

        std::vector<wf::ShotRow> shots(3);
        shots[0].timeMs = 10400;
        shots[0].shooter = muzzle.shooter;
        shots[1].timeMs = 10470;
        shots[1].shooter = muzzle.shooter;
        shots[2].timeMs = 10470;
        shots[2].shooter = 0x00050007u;
        Check(wf::ShotIndexFor(shots, muzzle) == 1, "the nearest shot of the same shooter");
        bad = muzzle;
        bad.shooter = wf::kNoId;
        Check(wf::ShotIndexFor(shots, bad) == -1, "no shooter, no shot");
        shots.push_back(shots[1]);
        Check(wf::DistinctShots(shots) == 3, "a pellet is not another shot");

        wf::SoundWindow sound;
        sound.after = 4;
        sound.expected = 4;
        Check(!wf::ExtraSound(sound), "as many weapon sounds as the recording: none extra");
        sound.after = 8;
        Check(wf::ExtraSound(sound), "twice the recording's: the shot doubled");
        sound.after = 4;
        sound.inCall = 1;
        Check(wf::ExtraSound(sound), "a sound made inside the call is extra");
        sound.inCall = 0;
        sound.expected = -1;
        sound.after = 9;
        Check(!wf::ExtraSound(sound), "a clip without its census judges only the call");
        sound.measured = false;
        sound.inCall = 3;
        Check(!wf::ExtraSound(sound), "census off: nothing judged");

        wf::PlayTotals totals;
        totals.calls = 4;
        totals.shotsWithExtraSound = 1;
        const std::string line = totals.Line();
        Check(line.find("WEAPON_FX_INTENT_PLAY_TOTAL,calls=4,") == 0 && line.find(",shots_with_extra_sound=1,") != std::string::npos &&
                  line.find(",shots_recorded=") != std::string::npos,
            "the total line carries calls, created, failed and the shots with extra sound");
    }
}

int RunFxResidueTests()
{
    TestWeaponFxPlay();
    TestRecreateWallRate();
    TestGapsP5();
    TestWeaponBonesB();
    TestShotAuditD();
    return g_residueFailures;
}
