// The pure parts of the particle replay (relocate mark, call gaps,
// recreation, the fx check's flags, the shot audit), with the numbers of
// recorded clips, the probe and a gunfight.
#include "fx_check_core.h"
#include "fx_motion_rule.h"
#include "qa_health.h"
#include "registered_fx_rule.h"
#include "shot_fx_audit_core.h"
#include "entity_events.h"

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace
{
    int g_particleFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_particleFailures;
    }

    namespace fxm = replay::fxmotion;
    using replay::entity::RegisteredFxGap;

    void TestShouldClearRelocate()
    {
        // The horse at a gallop moves ~0.12 m per frame; our instance has the
        // init's 10 m (0x29D6179), the hoof site's 1 m (0x146C1F9).
        Check(fxm::ShouldClearRelocate(true, 0.12f, 10.0f), "a gallop step under the limit clears");
        Check(fxm::ShouldClearRelocate(true, 0.12f, 1.0f), "a gallop step under the hoof site's limit clears");
        Check(!fxm::ShouldClearRelocate(true, 1.5f, 10.0f), "1.5 m is over the 1 m cap: a jump keeps the mark");
        Check(!fxm::ShouldClearRelocate(true, 0.6f, 0.5f), "over the instance's own limit keeps the mark");
        Check(!fxm::ShouldClearRelocate(false, 0.01f, 10.0f), "a seek, scrub or pause keeps the mark");
        Check(!fxm::ShouldClearRelocate(true, -1.0f, 10.0f), "a first placement (no step) keeps the mark");
        Check(fxm::ShouldClearRelocate(true, 0.2f, 0.0f), "an unread limit falls back to 1 m");
    }

    void TestCallGaps()
    {
        // A hoof track sampled per call at ~16 ms with two holes of a stride.
        std::vector<std::uint32_t> times;
        std::uint32_t t = 1000;
        for (int n = 0; n < 20; ++n, t += 16)
            times.push_back(t);
        const std::uint32_t holeFrom = times.back();
        t += 50; // no call for 66 ms
        for (int n = 0; n < 20; ++n, t += 16)
            times.push_back(t);
        const std::uint32_t secondFrom = times.back();
        t += 34;
        for (int n = 0; n < 10; ++n, t += 16)
            times.push_back(t);
        const std::vector<RegisteredFxGap> gaps = fxm::DeriveCallGaps(times);
        Check(gaps.size() == 2, "two holes of the hoof track become two stretches");
        Check(gaps.size() == 2 && gaps[0].fromMs == holeFrom && gaps[0].toMs == holeFrom + 66, "first stretch bounds");
        Check(gaps.size() == 2 && gaps[1].fromMs == secondFrom, "second stretch start");
        // A still object's smoke stores a frame only now and then: continuous.
        const std::vector<std::uint32_t> sparse = {0, 400, 900, 1500, 2100, 2600};
        Check(fxm::DeriveCallGaps(sparse).empty(), "a sparse track stays continuous");
        Check(fxm::DeriveCallGaps({0, 16, 32}).empty(), "too few frames: nothing derived");

        const std::vector<RegisteredFxGap> one = {{1000, 1066}};
        Check(fxm::InGap(one, 1030, 1014), "inside the stretch");
        Check(!fxm::InGap(one, 1000, 984), "at its start the call happened");
        Check(!fxm::InGap(one, 1080, 1066), "after it");
        Check(fxm::InGap(one, 1100, 990), "a tick that stepped over the whole stretch still skips once");
        Check(!fxm::InGap(one, 1100, 1030), "a tick after one already inside does not skip again");
        Check(!fxm::InGap(one, 990, 1100), "a jump back only checks containment");
    }

    void TestRecreate()
    {
        Check(fxm::ClassifyRecreate(false, false, true) == fxm::Recreate::First, "first creation");
        Check(fxm::ClassifyRecreate(true, true, true) == fxm::Recreate::InSpan, "made again in the same stretch (C1)");
        Check(fxm::ClassifyRecreate(true, false, true) == fxm::Recreate::Reentered, "after a jump: expected");
        Check(fxm::ClassifyRecreate(true, true, false) == fxm::Recreate::OwnerNotReady, "owner not ready");
        Check(fxm::RecreateHeld(1100, 1000, 250), "inside the backoff");
        Check(!fxm::RecreateHeld(1300, 1000, 250), "after the backoff");
        Check(!fxm::RecreateHeld(1100, 1000, 0), "backoff 0: off");
        Check(!fxm::RecreateHeld(1100, 0, 250), "never recreated");
        const float axis[3] = {0.0f, 1.0f, 0.0f};
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        Check(fxm::OwnerReady(true, true, axis), "ready owner");
        Check(!fxm::OwnerReady(true, false, axis), "no draw data: the attach update frees the entry");
        Check(!fxm::OwnerReady(true, true, zero), "a zero axis: freed");
        Check(!fxm::OwnerReady(false, true, axis), "no entity: freed");
    }

    void TestVelocity()
    {
        const float a[3] = {0.0f, 0.0f, 0.0f};
        const float b[3] = {0.12f, 0.0f, 0.0f};
        float v[3];
        fxm::StepVelocity(a, b, 1.0f / 60.0f, v);
        Check(v[0] > 7.1f && v[0] < 7.3f, "0.12 m in a 60 Hz frame is ~7.2 m/s (the 1152 gallop)");
        const float away[3] = {30.0f, 0.0f, 0.0f};
        fxm::StepVelocity(a, away, 1.0f / 60.0f, v);
        Check(v[0] == 0.0f, "a jump (30 m in a frame) is not motion");
        fxm::StepVelocity(a, b, 0.0f, v);
        Check(v[0] == 0.0f, "no time: no velocity");
        Check(fxm::StepMetres(a, b) > 0.119f && fxm::StepMetres(a, b) < 0.121f, "step metres");
    }

    void TestAfterEndAttached()
    {
        using replay::regfx::AfterEnd;
        using replay::regfx::Identity;
        Identity atEnd;
        atEnd.rule = 0x10;
        atEnd.hash = 0x6073ad36;
        atEnd.state = 2;
        atEnd.position[0] = 100.0f;
        atEnd.offset[2] = 0.05f;
        Identity now = atEnd;
        now.position[0] = 107.0f; // one second later on a galloping horse
        now.state = 3;
        Check(replay::regfx::ClassifyAfterEnd(true, atEnd, now, false) == AfterEnd::Reused,
            "by world position an attached instance on a moving horse looked reused");
        Check(replay::regfx::ClassifyAfterEnd(true, atEnd, now, false, true) == AfterEnd::Finishing,
            "by its offset on the bone it is ours, finishing");
        now.offset[2] = 0.4f;
        Check(replay::regfx::ClassifyAfterEnd(true, atEnd, now, false, true) == AfterEnd::Reused,
            "a new offset is a new tenant");
    }

    using replay::fxcheck::Tally;

    bool HasFlag(const std::vector<replay::fxcheck::Flag>& flags, const char* kind, std::uint32_t hash)
    {
        for (const auto& flag : flags)
            if (flag.kind == kind && flag.hash == hash)
                return true;
        return false;
    }

    void TestFxCheckFlags()
    {
        Tally tally;
        tally.Reset();
        // 1146: the chimney smoke 0x5cdc744c made again 349 times from 89 ms
        // on (every tick for ~6 s), one track.
        for (std::uint32_t n = 0; n < 349; ++n)
            tally.NoteRecreate(0x5cdc744c, 0, 7, 89 + n * 17);
        // A hoof track made again 3 times in 10 s after jumps: expected.
        tally.NoteRecreate(0x4c932de, 1, 9, 1000);
        tally.NoteRecreate(0x4c932de, 1, 9, 6000);
        // 1153: 0x6073ad36 relocated in 20 of 22 playback samples, no recording.
        for (int n = 0; n < 22; ++n)
            tally.NoteMotion(0x6073ad36, false, n < 20 ? 0x12u : 0x10u);
        // 0x10fd19c6, not attached: the moved bit only.
        for (int n = 0; n < 12; ++n)
            tally.NoteMotion(0x10fd19c6, false, 0x10u);
        auto flags = tally.Flags();
        Check(HasFlag(flags, "recreate_loop", 0x5cdc744c), "1146: the chimney smoke is a recreate loop");
        Check(!HasFlag(flags, "recreate_loop", 0x4c932de), "reentries after jumps are not a loop");
        Check(HasFlag(flags, "relocate_every_frame", 0x6073ad36), "1153: relocated every frame");
        Check(!HasFlag(flags, "relocate_every_frame", 0x10fd19c6), "the free effect is not relocated");

        // 1152: the recording's game instances moved in 763 of 1193 samples
        // (64%), relocated in none; the playback moved in none.
        Tally gallop;
        gallop.BeginRecord();
        for (int n = 0; n < 1193; ++n)
            gallop.NoteMotion(0x7125c257, true, n < 763 ? 0x10u : 0u);
        gallop.ResetPlay();
        for (int n = 0; n < 1000; ++n)
            gallop.NoteMotion(0x7125c257, false, 0x2u);
        // Restarts: 2936 in the recording, none in the playback (a stream).
        for (int n = 0; n < 2936; ++n)
            gallop.NoteRestart(0x7125c257, true);
        // Inherited velocity 0.12 m per frame recorded, 0 played.
        for (int n = 0; n < 20; ++n)
        {
            gallop.NoteVeladd(0x7125c257, true, 0.12f, 0.0f);
            gallop.NoteVeladd(0x7125c257, false, 0.0f, 7.4f);
        }
        flags = gallop.Flags();
        Check(HasFlag(flags, "relocate_every_frame", 0x7125c257), "1152: relocated in play, not in the recording");
        Check(HasFlag(flags, "motion_lost", 0x7125c257), "1152: motion lost");
        Check(HasFlag(flags, "restart_mismatch", 0x7125c257), "1152: restarts 0 against 2936");
        const std::string line = gallop.SummaryLine("1152-Colter.rpclip", true, "none");
        Check(line.find(",relocated_play=100.0,moved_rec=64.0,moved_play=0.0") != std::string::npos, "summary motion columns");
        Check(line.find(",restarts_rec=2936,restarts_play=0") != std::string::npos, "summary restart columns");
        const std::string report = gallop.Report("1152-Colter.rpclip", "v", "now", true, {});
        Check(report.find("PROBLEMS,4") != std::string::npos, "the report starts with its problems");
        Check(report.find("FX_CHECK_MOTION,clip=1152-Colter.rpclip,hash=0x7125c257") != std::string::npos, "motion row in the report");

        // The fixed playback: moved like the recording, restarts near, velocity back.
        gallop.ResetPlay();
        for (int n = 0; n < 1000; ++n)
            gallop.NoteMotion(0x7125c257, false, n < 620 ? 0x10u : 0u);
        for (int n = 0; n < 2800; ++n)
            gallop.NoteRestart(0x7125c257, false);
        for (int n = 0; n < 20; ++n)
        {
            gallop.NoteVeladd(0x7125c257, true, 0.12f, 0.0f);
            gallop.NoteVeladd(0x7125c257, false, 0.118f, 7.4f);
        }
        Check(gallop.Flags().empty(), "the fixed playback has no finding");

        // Copies, late owners, blinks, settings, weapons.
        Tally misc;
        misc.Reset();
        misc.NoteDupWorld(0x5cdc744c);
        misc.NoteOwnerLate(0x3df3fd46);
        misc.NoteBlink(0x1);
        misc.NoteSettings(0x2, true);
        misc.NoteSettings(0x2, false);
        misc.NoteShotAudit(2, 1, 0, false);
        misc.NotePlacement(5.0f, 3.0f);
        misc.NotePlacement(0.2f, 3.0f);
        misc.NoteWeapon(0x1452671, 0, 12);
        misc.NoteWeapon(0x1452671, 1, 12);
        misc.NoteWeapon(0x1452671, 2, 14);
        flags = misc.Flags();
        Check(HasFlag(flags, "dup_world", 0x5cdc744c) && HasFlag(flags, "owner_late", 0x3df3fd46) && HasFlag(flags, "blink", 0x1) &&
                HasFlag(flags, "settings_diff", 0x2) && HasFlag(flags, "weapon_shared", 0) && HasFlag(flags, "placement_far", 0),
            "copies, late owners, blinks, settings, shared weapon effects and placement are findings");
        Check(misc.PlacementChecked() == 2 && misc.PlacementFar() == 1, "placement counts");
        Check(misc.SummaryLine("x", false, "none").find(",weapon_extra=2,weapon_missing=0,weapon_live=12,shared=3") != std::string::npos,
            "weapon columns");
        const std::string flagLine = Tally::FlagLine("x", flags.front());
        Check(flagLine.find("FX_CHECK_FLAG,clip=x,kind=") == 0, "flag line head");
        // The QA health summary counts it (QA_HEALTH_SUMMARY fx_check_flag).
        replay::qa::Anomaly anomaly;
        Check(replay::qa::ClassifyLine(flagLine, 1234, anomaly) && anomaly.kind == replay::qa::Kind::FxCheckFlag &&
                anomaly.detail.find("kind=") == 0 && std::string(replay::qa::KindText(anomaly.kind)) == "fx_check_flag",
            "an fx check finding is a QA anomaly");
    }

    void TestShotAudit()
    {
        namespace sf = replay::shotfx;
        sf::Audit audit;
        // The gunfight: the player at 10471 ms and another shooter at 10488 ms
        // took the same muzzle and impact triggers.
        std::vector<sf::Shot> shots(3);
        shots[0] = {10471, 0x10001, true, 0xAAAA, 0xBBBB, true};
        shots[1] = {10488, 0x20003, false, 0xAAAA, 0xBBBB, true};
        shots[2] = {12000, 0x20003, false, 0xAAAA, 0xCCCC, true};
        std::vector<sf::Decision> d = sf::Decide(shots, {}, true, audit);
        Check(audit.muzzleShared == 2 && audit.impactShared == 2, "two shots share the muzzle and the impact");
        Check(d[0].playMuzzle && d[0].playImpact, "the player's shot keeps them");
        Check(!d[1].playMuzzle && !d[1].playImpact, "the other shooter's copy is not played");
        Check(d[2].playMuzzle && d[2].playImpact, "a shot 1.5 s later plays its own");
        Check(audit.legacySkippedMuzzle == 1 && audit.legacySkippedImpact == 1, "skips counted");
        Check(std::string(audit.Verdict()) == "legacy_shared", "verdict");

        // The probe: 4 of the shots hit a body (body effect within -30..+10 ms).
        sf::Audit probe;
        std::vector<sf::Shot> probeShots;
        for (std::uint32_t n = 0; n < 22; ++n)
            probeShots.push_back({1000 + n * 400, 0x10001, true, n < 12 ? 0xAAAAu : 0u, n < 16 ? 0xBBBBu : 0u, n < 16});
        const std::vector<std::uint32_t> body = {1000 + 3 * 400 - 20, 1000 + 5 * 400 + 5, 1000 + 7 * 400, 1000 + 9 * 400 - 30};
        d = sf::Decide(probeShots, body, true, probe);
        Check(probe.shots == 22 && probe.muzzle == 12 && probe.impact == 16, "probe counts (22 shots, 12 flashes, 16 impacts)");
        Check(probe.impactOnBody == 4 && !d[3].playImpact && !d[5].playImpact && d[4].playImpact, "4 surface impacts on a body skipped");
        Check(probe.muzzleShared == 0, "one shooter: nothing shared");

        // Without the rule: decided as usual, only counted.
        sf::Audit plain;
        d = sf::Decide(shots, {}, false, plain);
        Check(d[1].playMuzzle && plain.muzzleShared == 2 && plain.legacySkippedMuzzle == 0, "dedupe off: counted, played");
        // Pellets: same shooter, same ms.
        std::vector<sf::Shot> shotgun = {{500, 1, false, 0xA, 0xB, true}, {500, 1, false, 0xA, 0xC, true}};
        sf::Audit pellets;
        sf::Decide(shotgun, {}, true, pellets);
        Check(pellets.pelletShots == 2 && pellets.muzzleShared == 0, "pellets are not shared claims");
        sf::Audit events;
        events.weaponEvents = true;
        Check(std::string(events.Verdict()) == "events" && events.Line("c").find("weapon_events=1") != std::string::npos,
            "a clip with the weapon events");
    }

    // The flags do not depend on the backoff, the motion is judged without
    // a recording, the restart basis is per span.
    void TestFxCheckMeasureReview()
    {
        Tally slow;
        slow.Reset();
        // A loop held by a backoff of 400 ms: 2.4 a second for 10 s.
        for (std::uint32_t n = 0; n < 24; ++n)
            slow.NoteRecreate(0x5cdc744c, 0, 3, 100 + n * 417);
        Check(HasFlag(slow.Flags(), "recreate_loop", 0x5cdc744c), "a loop slowed by the backoff is still a loop");
        // Twice in 10 s (after an owner came back): not a loop.
        Tally rare;
        rare.Reset();
        rare.NoteRecreate(0x1, 0, 1, 1000);
        rare.NoteRecreate(0x1, 0, 1, 9000);
        Check(!HasFlag(rare.Flags(), "recreate_loop", 0x1), "two recreations in 8 s are not a loop");

        // 1153 played again: the hoof dust on a galloping horse never moved
        // for the engine (bit 0x10 in 0 of 22), no recording samples:
        // motion_lost from the moving owner alone.
        Tally other;
        other.Reset();
        for (int n = 0; n < 22; ++n)
            other.NoteMotion(0x6073ad36, false, 0x0u, true);
        // The same effect standing (owner still): no finding.
        for (int n = 0; n < 30; ++n)
            other.NoteMotion(0x10fd19c6, false, 0x0u, false);
        auto flags = other.Flags();
        Check(HasFlag(flags, "motion_lost", 0x6073ad36), "moving owner, the engine never saw the effect move");
        Check(!HasFlag(flags, "motion_lost", 0x10fd19c6), "a still owner is not motion lost");
        Tally fixed;
        fixed.Reset();
        for (int n = 0; n < 22; ++n)
            fixed.NoteMotion(0x6073ad36, false, n < 14 ? 0x10u : 0x0u, true);
        Check(!HasFlag(fixed.Flags(), "motion_lost", 0x6073ad36), "moved in 64% while the owner moves: fine");

        // Restart basis: the gaps due in the traversed spans win over the
        // whole recording's restarts (a stop halfway).
        Tally half;
        half.BeginRecord();
        for (int n = 0; n < 2936; ++n)
            half.NoteRestart(0x7125c257, true);
        half.ResetPlay();
        half.ClearRecGaps();
        half.NoteGap(0x7125c257, true, 1400); // half of the clip traversed
        for (int n = 0; n < 1380; ++n)
            half.NoteRestart(0x7125c257, false);
        Check(!HasFlag(half.Flags(), "restart_mismatch", 0x7125c257), "a stop halfway is not a restart mismatch");
        half.ClearRecGaps();
        Check(HasFlag(half.Flags(), "restart_mismatch", 0x7125c257), "without the gaps the whole recording is the basis");
    }

    // The settings never carry a value that is not a number into an
    // instance, and +0x3BD (runtime) is not compared.
    void TestFxSettingsCrashReview()
    {
        using replay::entity::FxSettings;
        FxSettings s;
        s.mask = FxSettings::kApplied;
        s.tele3A8 = std::numeric_limits<float>::quiet_NaN();
        s.lod3A0 = std::numeric_limits<float>::infinity();
        s.vel350[2] = -std::numeric_limits<float>::infinity();
        s.alpha38C = 0.5f;
        replay::entity::FxSettingsKeepFinite(s);
        Check(!(s.mask & FxSettings::kTele3A8), "a NaN distance is not written back");
        Check(!(s.mask & FxSettings::kLod3A0), "an infinite field is not written back");
        Check(!(s.mask & FxSettings::kVel350), "one infinite lane drops the whole +0x350 vector");
        Check((s.mask & FxSettings::kAlpha38C) && (s.mask & FxSettings::kCull3BC), "finite fields stay");
        FxSettings a, b;
        a.cull3BC = 0x0101;
        b.cull3BC = 0x0001; // the engine zeroed +0x3BD (0x29ED5D9)
        Check(replay::entity::FxSettingsDiff(a, b, FxSettings::kCull3BC) == 0, "+0x3BD alone is not a settings difference");
        b.cull3BC = 0x0100;
        Check(replay::entity::FxSettingsDiff(a, b, FxSettings::kCull3BC) == FxSettings::kCull3BC, "+0x3BC differs");
    }
}

int RunParticleReplayTests()
{
    TestShouldClearRelocate();
    TestCallGaps();
    TestRecreate();
    TestVelocity();
    TestAfterEndAttached();
    TestFxCheckFlags();
    TestShotAudit();
    TestFxCheckMeasureReview();
    TestFxSettingsCrashReview();
    return g_particleFailures;
}
