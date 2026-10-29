#pragma once

// The fx check, the particle side of the sound check. Every particle instance
// the engine starts while recording and while playing is tallied by effect
// hash (one-shot starts by the trigger's hash, registered effects by their
// creations in the registered core), with who made it in playback: the
// replay's own players (ours) or the engine (on a puppet or the hidden
// player, or anywhere else). The comparison is per hash: what the recording
// saw against what the playback showed, as the record and play histograms of
// the bench (POSE_V2_FX_HISTOGRAM) but over every start, not the top 24.
//
// Pure (no game); unit tested.

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace replay::fxcheck
{
    // Who started an instance in playback.
    enum class Maker : std::uint8_t
    {
        Ours,         // the replay's fx tracks
        EnginePuppet, // the engine, attached to (or registered on) a puppet or the hidden player
        EngineOther,  // the engine, anywhere else (the replay's explosions and fires end here too)
    };

    // How a recording start ended in the clip.
    enum class Stored : std::uint8_t
    {
        Stored,      // in the one-shot fx track
        Shot,        // left to the shot track (muzzle, impact of a recorded shot)
        Skipped,     // attached to an actor the recording does not have
        Unresolved,  // no world position
        Registered,  // a registered effect (counted by its creation)
    };

    struct Row
    {
        std::uint32_t rec = 0;        // starts (one-shot) or creations (registered) while recording
        std::uint32_t recStored = 0;  // ...kept in the clip's one-shot track
        std::uint32_t recShot = 0;
        std::uint32_t recSkipped = 0;
        std::uint32_t recUnresolved = 0;
        std::uint32_t clip = 0;       // in the clip (one-shot events, registered tracks)
        std::uint32_t playOurs = 0;
        std::uint32_t playEnginePuppet = 0;
        std::uint32_t playEngineOther = 0;
        std::uint32_t refused = 0;    // engine starts refused on puppets (the owner rule)
        bool registered = false;
        std::uint32_t site = 0;       // a caller site seen (evidence)

        // What the counts above cannot see (the defects are visual).
        // Recreation of our registered instance (fx_motion_rule.h Recreate).
        std::uint32_t recreateInSpan = 0;
        std::uint32_t reentered = 0;
        std::uint32_t ownerNotReady = 0;
        // Motion: samples of +0x584 of the live attached instances (the
        // game's while recording, ours while playing), with the relocate bit
        // 0x2 and the moved bit 0x10, and +0x360 (inherited velocity per
        // frame, metres) every 100 ms.
        std::uint32_t recMotion = 0;
        std::uint32_t recRelocated = 0;
        std::uint32_t recMoved = 0;
        std::uint32_t playMotion = 0;
        std::uint32_t playRelocated = 0;
        std::uint32_t playMoved = 0;
        // The playback samples taken while the owner moved (published speed
        // above kMovingOwnerMps) and how many of them had the moved bit 0x10.
        // That bit is the engine's own verdict of the frame (0x29F416C: set
        // when the matrix changed, after the relocate handler 0x29E0A1C copied
        // it into the previous one), so it is the honest motion signal; bit
        // 0x2 sampled after our own clear only shows what the replay left
        // (relocate_every_frame).
        std::uint32_t playMovingSamples = 0;
        std::uint32_t playMovingMoved = 0;
        std::vector<float> recVeladd;
        std::vector<float> playVeladd;
        std::vector<float> playOwnerSpeed;
        // Stops and restarts of the registry: stretches without a call
        // and restarts from 0x2C5268F, calls against the ticks in force.
        std::uint32_t recGaps = 0;
        std::uint32_t recRestarts = 0;
        std::uint32_t playGaps = 0;
        std::uint32_t playRestarts = 0;
        std::uint64_t recCalled = 0;
        std::uint64_t recTicks = 0;
        std::uint64_t playCalled = 0;
        std::uint64_t playTicks = 0;
        bool gapsDerived = false; // the clip has no RGSS: the stretches come from the frame times
        // Copies and late owners, blinks and settings.
        std::uint32_t dupWorld = 0;
        std::uint32_t ownerLate = 0;
        std::uint32_t blinks = 0;
        std::uint32_t settingsChecked = 0;
        std::uint32_t settingsDiff = 0;

        std::uint32_t Play() const { return playOurs + playEnginePuppet + playEngineOther; }
    };

    // The weapon sites (muzzle 0x1452671, impact 0x144D33A, body 0x144C076, the
    // others of the weapon vfx) by phase.
    struct WeaponRow
    {
        std::uint32_t live = 0;   // starts while recording
        std::uint32_t clip = 0;   // events in the clip (or shots that carry the hash, legacy)
        std::uint32_t played = 0; // played by the replay
    };

    // A finding the bench fails on (FX_CHECK_FLAG).
    struct Flag
    {
        std::string kind;
        std::uint32_t hash = 0;
        double value = 0.0;
        double limit = 0.0;
    };

    constexpr std::size_t kMaxMotionValues = 512;
    // Thresholds of Flags(), calibrated on recorded logs (a hypothesis).
    constexpr double kRecreateLoopPerSecond = 1.0;
    constexpr float kMovingOwnerMps = 0.5f;
    constexpr double kMovedWhileMovingPct = 10.0;

    inline double Median(std::vector<float> values)
    {
        if (values.empty())
            return 0.0;
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2), values.end());
        return values[values.size() / 2];
    }
    inline double Pct(std::uint64_t part, std::uint64_t whole) { return whole ? 100.0 * double(part) / double(whole) : 0.0; }

    struct Summary
    {
        bool rawBasis = false; // the recording's own starts (same session) or the clip's content
        std::uint64_t rec = 0;
        std::uint64_t play = 0;
        std::uint64_t matched = 0;
        std::uint64_t missing = 0;
        std::uint64_t extra = 0;
        std::uint64_t ours = 0;
        std::uint64_t enginePuppet = 0;
        std::uint64_t engineOther = 0;
        std::uint64_t refused = 0;
        std::uint32_t effects = 0;
        double MissingPct() const { return rec ? 100.0 * double(missing) / double(rec) : 0.0; }
        double ExtraPct() const { return rec ? 100.0 * double(extra) / double(rec) : 0.0; }
    };

    class Tally
    {
    public:
        void Reset()
        {
            rows_.clear();
            recSeen_ = false;
            ResetPlayExtras();
            weapons_.clear();
        }
        void ResetPlay()
        {
            for (auto& entry : rows_)
            {
                Row& row = entry.second;
                row.clip = 0;
                row.playOurs = row.playEnginePuppet = row.playEngineOther = 0;
                row.refused = 0;
                row.recreateInSpan = row.reentered = row.ownerNotReady = 0;
                row.playMotion = row.playRelocated = row.playMoved = 0;
                row.playMovingSamples = row.playMovingMoved = 0;
                row.playVeladd.clear();
                row.playOwnerSpeed.clear();
                row.playGaps = row.playRestarts = 0;
                row.playCalled = row.playTicks = 0;
                row.gapsDerived = false;
                row.dupWorld = row.ownerLate = row.blinks = row.settingsChecked = row.settingsDiff = 0;
            }
            for (auto& entry : weapons_)
                entry.second.clip = entry.second.played = 0;
            ResetPlayExtras();
        }
        void BeginRecord()
        {
            rows_.clear();
            weapons_.clear();
            ResetPlayExtras();
            recSeen_ = true;
        }
        bool HasRecord() const { return recSeen_; }
        void ForgetRecord() { recSeen_ = false; }

        void NoteRecord(std::uint32_t hash, Stored how, bool registered, std::uint32_t site)
        {
            Row& row = rows_[hash];
            ++row.rec;
            row.registered = row.registered || registered;
            if (!row.site)
                row.site = site;
            switch (how)
            {
            case Stored::Stored: ++row.recStored; break;
            case Stored::Shot: ++row.recShot; break;
            case Stored::Skipped: ++row.recSkipped; break;
            case Stored::Unresolved: ++row.recUnresolved; break;
            case Stored::Registered: break;
            }
        }
        // The clip's counts again, for the part the playback reached.
        void ClearClip()
        {
            for (auto& entry : rows_)
                entry.second.clip = 0;
        }
        // The basis of restart_mismatch is counted again at the stop, per
        // traversed span (a stop halfway or a part played twice changes the
        // restarts due), like the clip basis.
        void ClearRecGaps()
        {
            for (auto& entry : rows_)
                entry.second.recGaps = 0;
        }
        void NoteClip(std::uint32_t hash, bool registered, std::uint32_t count = 1)
        {
            Row& row = rows_[hash];
            row.clip += count;
            row.registered = row.registered || registered;
        }
        void NotePlay(std::uint32_t hash, Maker maker, bool registered, std::uint32_t site)
        {
            Row& row = rows_[hash];
            row.registered = row.registered || registered;
            if (!row.site)
                row.site = site;
            switch (maker)
            {
            case Maker::Ours: ++row.playOurs; break;
            case Maker::EnginePuppet: ++row.playEnginePuppet; break;
            case Maker::EngineOther: ++row.playEngineOther; break;
            }
        }
        void NoteRefused(std::uint32_t hash) { ++rows_[hash].refused; }

        // kind: 0 in span, 1 reentered, 2 owner not ready (fx_motion_rule.h).
        // wallMs: the wall clock of the recreation (0: unknown); with it the
        // rate of recreate_loop is per wall second, not per second of playhead
        // (a playhead that stands still can see dozens of recreations).
        void NoteRecreate(std::uint32_t hash, int kind, std::size_t track, std::uint32_t playheadMs, std::uint64_t wallMs = 0)
        {
            Row& row = rows_[hash];
            row.registered = true;
            if (kind == 1)
            {
                ++row.reentered;
                return;
            }
            if (kind == 2)
                ++row.ownerNotReady;
            else
                ++row.recreateInSpan;
            TrackRecreate& t = recreates_[track];
            t.hash = hash;
            if (!t.count++)
            {
                t.firstMs = playheadMs;
                t.firstWallMs = wallMs;
            }
            t.lastMs = (std::max)(t.lastMs, playheadMs);
            if (wallMs)
            {
                if (!t.firstWallMs)
                    t.firstWallMs = wallMs;
                t.lastWallMs = (std::max)(t.lastWallMs, wallMs);
            }
        }
        void NoteMotion(std::uint32_t hash, bool record, std::uint32_t flags2, bool ownerMoving = false)
        {
            Row& row = rows_[hash];
            if (record)
            {
                ++row.recMotion;
                row.recRelocated += (flags2 & 0x2u) ? 1 : 0;
                row.recMoved += (flags2 & 0x10u) ? 1 : 0;
            }
            else
            {
                ++row.playMotion;
                row.playRelocated += (flags2 & 0x2u) ? 1 : 0;
                row.playMoved += (flags2 & 0x10u) ? 1 : 0;
                if (ownerMoving)
                {
                    ++row.playMovingSamples;
                    row.playMovingMoved += (flags2 & 0x10u) ? 1 : 0;
                }
            }
        }
        void NoteVeladd(std::uint32_t hash, bool record, float metresPerFrame, float ownerSpeed)
        {
            Row& row = rows_[hash];
            std::vector<float>& values = record ? row.recVeladd : row.playVeladd;
            if (values.size() < kMaxMotionValues)
                values.push_back(metresPerFrame);
            if (!record && row.playOwnerSpeed.size() < kMaxMotionValues)
                row.playOwnerSpeed.push_back(ownerSpeed);
        }
        void NoteGap(std::uint32_t hash, bool record, std::uint32_t count = 1)
        {
            Row& row = rows_[hash];
            (record ? row.recGaps : row.playGaps) += count;
        }
        void NoteGapsDerived(std::uint32_t hash) { rows_[hash].gapsDerived = true; }
        void NoteRestart(std::uint32_t hash, bool record) { ++(record ? rows_[hash].recRestarts : rows_[hash].playRestarts); }
        void NoteCall(std::uint32_t hash, bool record, bool called)
        {
            Row& row = rows_[hash];
            if (record)
            {
                ++row.recTicks;
                row.recCalled += called ? 1 : 0;
            }
            else
            {
                ++row.playTicks;
                row.playCalled += called ? 1 : 0;
            }
        }
        void NoteDupWorld(std::uint32_t hash) { ++rows_[hash].dupWorld; }
        void NoteOwnerLate(std::uint32_t hash) { ++rows_[hash].ownerLate; }
        void NoteBlink(std::uint32_t hash) { ++rows_[hash].blinks; }
        void NoteSettings(std::uint32_t hash, bool differs)
        {
            Row& row = rows_[hash];
            ++row.settingsChecked;
            row.settingsDiff += differs ? 1 : 0;
        }
        // phase: 0 live (recording), 1 clip, 2 played.
        void NoteWeapon(std::uint32_t site, int phase, std::uint32_t count = 1)
        {
            WeaponRow& row = weapons_[site];
            (phase == 0 ? row.live : phase == 1 ? row.clip : row.played) += count;
        }
        void NotePlacement(float distanceM, float farM)
        {
            ++placementChecked_;
            if (distanceM > farM)
                ++placementFar_;
            placementMaxM_ = (std::max)(placementMaxM_, distanceM);
        }
        void NoteImpactOnBody() { ++impactOnBody_; }
        void NoteShotAudit(std::uint32_t muzzleShared, std::uint32_t impactShared, std::uint32_t onBody, bool weaponEvents)
        {
            shotShared_ = muzzleShared + impactShared;
            shotOnBody_ = onBody;
            weaponEvents_ = weaponEvents;
        }
        // The shots of the traversed spans without a muzzle effect of their own
        // (weapon_fx_intent_core.h AuditShots; culled: the weapon system asked,
        // the engine made none) and the weapons with a recorded pose of their
        // bones reached and armed by the playback.
        void NoteShots(std::uint32_t noMuzzle, std::uint32_t culled)
        {
            shotNoFx_ = noMuzzle;
            shotCulled_ = culled;
        }
        void NoteWeaponBones(std::uint32_t seen, std::uint32_t armed)
        {
            weaponBonesSeen_ = seen;
            weaponBonesArmed_ = armed;
        }
        // The armed weapon poses the engine drew over (weapon_bones_core.h
        // Verdict) in most ticks with motion, or in part. Frozen: weapons with
        // a bone the record turns and the drawing does not.
        void NoteWeaponBonesFrozen(std::uint32_t frozen) { weaponBonesFrozen_ = frozen; }
        // Weapons read back (weapon_look_core.h), and the ones whose last check
        // disagreed with the clip on components / wear.
        void NoteWeaponLook(std::uint32_t checked, std::uint32_t componentsMismatch, std::uint32_t wearMismatch)
        {
            weaponLookChecked_ = checked;
            weaponComponentsMismatch_ = componentsMismatch;
            weaponWearMismatch_ = wearMismatch;
        }
        void NoteWeaponBonesDrawn(std::uint32_t overwritten, std::uint32_t mixed, std::uint32_t shown)
        {
            weaponBonesOverwritten_ = overwritten;
            weaponBonesMixed_ = mixed;
            weaponBonesShown_ = shown;
        }
        std::uint32_t PlacementChecked() const { return placementChecked_; }
        std::uint32_t PlacementFar() const { return placementFar_; }
        float PlacementMaxM() const { return placementMaxM_; }
        const std::map<std::uint32_t, WeaponRow>& Weapons() const { return weapons_; }

        // The findings (FX_CHECK_FLAG): recreate_loop (a track made again
        // more than 3 times a second in one stretch), relocate_every_frame
        // (relocated in more than half of the playback samples while the
        // recording shows it in under 5%, or no recording: the rule), motion_lost
        // (moved under half of the recording's, or the inherited velocity under
        // 0.2 of the recording's), restart_mismatch (restarts more than 20% off
        // the recording's), dup_world, owner_late, blink, settings_diff,
        // weapon_shared and weapon_on_body (an old clip's shot track),
        // placement_far (a weapon effect away from where it should be),
        // shot_no_fx (shots without a muzzle effect of their own) and
        // weapon_bones_static (a weapon pose recorded and never armed).
        std::vector<Flag> Flags() const
        {
            std::vector<Flag> out;
            for (const auto& [track, t] : recreates_)
            {
                (void)track;
                const double span = t.firstWallMs && t.lastWallMs >= t.firstWallMs ? double(t.lastWallMs - t.firstWallMs)
                                                                                    : double(t.lastMs - t.firstMs);
                const double seconds = (std::max)(1.0, span / 1000.0);
                const double rate = double(t.count) / seconds;
                // The limit does not sit under the recreate backoff (250 ms
                // caps a loop at ~3.7 a second, a backoff of 400 ms at 2.5,
                // both a visible blink): a continuous registered effect is
                // never made again in the same stretch except after a stretch
                // without a call (counted apart), so one a second is already a
                // loop.
                if (t.count > 3 && rate > kRecreateLoopPerSecond)
                    out.push_back({"recreate_loop", t.hash, rate, kRecreateLoopPerSecond});
            }
            for (const auto& [hash, row] : rows_)
            {
                if (row.playMotion >= 10)
                {
                    const double relPlay = Pct(row.playRelocated, row.playMotion);
                    const bool recKnown = row.recMotion >= 10;
                    const double relRec = recKnown ? Pct(row.recRelocated, row.recMotion) : 0.0;
                    if (relPlay > 50.0 && relRec < 5.0)
                        out.push_back({"relocate_every_frame", hash, relPlay, 50.0});
                    if (recKnown)
                    {
                        const double movedRec = Pct(row.recMoved, row.recMotion);
                        const double movedPlay = Pct(row.playMoved, row.playMotion);
                        if (movedRec >= 10.0 && movedPlay < 0.5 * movedRec)
                            out.push_back({"motion_lost", hash, movedPlay, 0.5 * movedRec});
                    }
                    else if (row.playMovingSamples >= 10)
                    {
                        // A clip played in another session has no recording
                        // samples; an effect on an owner that moves and that
                        // the engine almost never saw move (bit 0x10) lost its
                        // motion.
                        const double movedMoving = Pct(row.playMovingMoved, row.playMovingSamples);
                        if (movedMoving < kMovedWhileMovingPct)
                            out.push_back({"motion_lost", hash, movedMoving, kMovedWhileMovingPct});
                    }
                }
                if (row.recVeladd.size() >= 5 && row.playVeladd.size() >= 5)
                {
                    const double rec = Median(row.recVeladd);
                    const double play = Median(row.playVeladd);
                    if (rec > 0.01 && play < 0.2 * rec)
                        out.push_back({"motion_lost", hash, play, 0.2 * rec});
                }
                // The recording's restarts (same session) or the clip's
                // stretches without a call (RGSS, or derived from the frames).
                // The stretches due in the traversed spans first
                // (EndPoseV2FxCheck counts them per span), the recording's
                // whole restart count only without them.
                const std::uint32_t restartBasis = row.recGaps ? row.recGaps : row.recRestarts;
                if (restartBasis >= 5)
                {
                    const double off = std::fabs(double(row.playRestarts) - double(restartBasis));
                    if (off > 0.2 * restartBasis)
                        out.push_back({"restart_mismatch", hash, double(row.playRestarts), double(restartBasis)});
                }
                if (row.dupWorld)
                    out.push_back({"dup_world", hash, double(row.dupWorld), 0.0});
                if (row.ownerLate)
                    out.push_back({"owner_late", hash, double(row.ownerLate), 0.0});
                if (row.blinks)
                    out.push_back({"blink", hash, double(row.blinks), 0.0});
                if (row.settingsDiff)
                    out.push_back({"settings_diff", hash, double(row.settingsDiff), 0.0});
            }
            if (!weaponEvents_ && shotShared_)
                out.push_back({"weapon_shared", 0, double(shotShared_), 0.0});
            if (impactOnBody_)
                out.push_back({"weapon_on_body", 0, double(impactOnBody_), 0.0});
            if (placementFar_)
                out.push_back({"placement_far", 0, double(placementFar_), 0.0});
            // A shot whose muzzle the engine culled while recording but whose
            // WFXI request the replay calls (culled) is not a finding: the
            // playback asks the weapon system again and the playback camera
            // decides (WEAPON_FX_INTENT_PLAY created_fx/in_view measure it).
            if (shotNoFx_ > shotCulled_)
                out.push_back({"shot_no_fx", 0, double(shotNoFx_ - shotCulled_), 0.0});
            if (weaponBonesArmed_ < weaponBonesSeen_)
                out.push_back({"weapon_bones_static", 0, double(weaponBonesSeen_ - weaponBonesArmed_), 0.0});
            // Armed and written, but not what was drawn (a reload played
            // still with every weapon armed).
            if (weaponBonesOverwritten_ + weaponBonesMixed_)
                out.push_back({"weapon_bones_overwritten", 0, double(weaponBonesOverwritten_ + weaponBonesMixed_), 0.0});
            if (weaponBonesFrozen_)
                out.push_back({"weapon_bones_frozen", 0, double(weaponBonesFrozen_), 0.0});
            if (weaponComponentsMismatch_)
                out.push_back({"weapon_components_mismatch", 0, double(weaponComponentsMismatch_), 0.0});
            if (weaponWearMismatch_)
                out.push_back({"weapon_wear_mismatch", 0, double(weaponWearMismatch_), 0.0});
            return out;
        }

        static std::string FlagLine(const std::string& clip, const Flag& flag)
        {
            std::ostringstream line;
            line.setf(std::ios::fixed);
            line.precision(2);
            line << "FX_CHECK_FLAG,clip=" << clip << ",kind=" << flag.kind << ",hash=" << Hex(flag.hash)
                 << ",value=" << flag.value << ",limit=" << flag.limit;
            return line.str();
        }

        // One FX_CHECK_MOTION line per hash with motion or restart data.
        std::vector<std::string> MotionLines(const std::string& clip) const
        {
            std::vector<std::string> out;
            const std::vector<Flag> flags = Flags();
            for (const auto& [hash, row] : rows_)
            {
                if (!row.playMotion && !row.recMotion && !row.playTicks && !row.recTicks)
                    continue;
                std::ostringstream line;
                line.setf(std::ios::fixed);
                line.precision(3);
                std::string verdict = "ok";
                for (const Flag& flag : flags)
                    if (flag.hash == hash && (flag.kind == "relocate_every_frame" || flag.kind == "motion_lost" ||
                                                 flag.kind == "restart_mismatch"))
                        verdict = flag.kind;
                line << "FX_CHECK_MOTION,clip=" << clip << ",hash=" << Hex(hash)
                     << ",owner_speed=" << Median(row.playOwnerSpeed)
                     << ",tele_rec_pct=" << Pct(row.recRelocated, row.recMotion)
                     << ",tele_play_pct=" << Pct(row.playRelocated, row.playMotion)
                     << ",moved_rec_pct=" << Pct(row.recMoved, row.recMotion)
                     << ",moved_play_pct=" << Pct(row.playMoved, row.playMotion)
                     << ",moved_moving_pct=" << Pct(row.playMovingMoved, row.playMovingSamples)
                     << ",samples_moving=" << row.playMovingSamples
                     << ",veladd_rec=" << Median(row.recVeladd) << ",veladd_play=" << Median(row.playVeladd)
                     << ",samples_rec=" << row.recMotion << ",samples_play=" << row.playMotion
                     << ",gaps_rec=" << row.recGaps << ",gaps_play=" << row.playGaps << ",derived=" << (row.gapsDerived ? 1 : 0)
                     << ",restarts_rec=" << row.recRestarts << ",restarts_play=" << row.playRestarts
                     << ",calls_rec_pct=" << Pct(row.recCalled, row.recTicks)
                     << ",calls_play_pct=" << Pct(row.playCalled, row.playTicks) << ",verdict=" << verdict;
                out.push_back(line.str());
            }
            return out;
        }

        const std::map<std::uint32_t, Row>& Rows() const { return rows_; }

        // rawBasis: the recording's own starts when this session recorded the
        // clip (HasRecord), the clip's content otherwise.
        Summary Summarize(bool rawBasis) const
        {
            Summary s;
            s.rawBasis = rawBasis;
            for (const auto& entry : rows_)
            {
                const Row& row = entry.second;
                const std::uint64_t rec = rawBasis ? row.rec : row.clip;
                const std::uint64_t play = row.Play();
                if (!rec && !play && !row.refused)
                    continue;
                ++s.effects;
                s.rec += rec;
                s.play += play;
                s.matched += std::min(rec, play);
                s.missing += rec > play ? rec - play : 0;
                s.extra += play > rec ? play - rec : 0;
                s.ours += row.playOurs;
                s.enginePuppet += row.playEnginePuppet;
                s.engineOther += row.playEngineOther;
                s.refused += row.refused;
            }
            return s;
        }

        static std::string Hex(std::uint32_t value)
        {
            std::ostringstream text;
            text << "0x" << std::hex << value;
            return text.str();
        }

        // The worst rows first: by missing, or by extra.
        std::vector<std::pair<std::uint32_t, std::int64_t>> Ranked(bool rawBasis, bool missing) const
        {
            std::vector<std::pair<std::uint32_t, std::int64_t>> out;
            for (const auto& entry : rows_)
            {
                const std::int64_t rec = rawBasis ? entry.second.rec : entry.second.clip;
                const std::int64_t gap = missing ? rec - std::int64_t(entry.second.Play())
                                                 : std::int64_t(entry.second.Play()) - rec;
                if (gap > 0)
                    out.push_back({entry.first, gap});
            }
            std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second > b.second : a.first < b.first;
            });
            return out;
        }

        std::string SummaryLine(const std::string& clip, bool rawBasis, const std::string& report) const
        {
            const Summary s = Summarize(rawBasis);
            std::ostringstream line;
            line.setf(std::ios::fixed);
            line.precision(1);
            line << "FX_CHECK,clip=" << clip << ",basis=" << (rawBasis ? "recording" : "clip") << ",effects=" << s.effects
                 << ",rec=" << s.rec << ",play=" << s.play << ",matched=" << s.matched << ",missing=" << s.missing
                 << ",missing_pct=" << s.MissingPct() << ",extra=" << s.extra << ",extra_pct=" << s.ExtraPct()
                 << ",ours=" << s.ours << ",engine_puppet=" << s.enginePuppet << ",engine_other=" << s.engineOther
                 << ",refused=" << s.refused;
            AppendExtras(line);
            line << ",top_missing=";
            AppendTop(line, Ranked(rawBasis, true));
            line << ",top_extra=";
            AppendTop(line, Ranked(rawBasis, false));
            line << ",report=" << report;
            return line.str();
        }

        std::string Report(const std::string& clip, const std::string& version, const std::string& when, bool rawBasis,
            const std::map<std::uint32_t, std::string>& names) const
        {
            const Summary s = Summarize(rawBasis);
            std::ostringstream out;
            out.setf(std::ios::fixed);
            out.precision(1);
            out << "fx check: " << clip << "\n";
            out << "version " << version << ", " << when << "\n";
            {
                // The findings first (the bench fails on any).
                const std::vector<Flag> flags = Flags();
                out << "\nPROBLEMS," << flags.size() << "\n";
                for (const Flag& flag : flags)
                    out << "  " << FlagLine(clip, flag) << "\n";
                if (flags.empty())
                    out << "  none\n";
                std::ostringstream extras;
                AppendExtras(extras);
                out << "COUNTERS" << extras.str() << "\n";
                for (const std::string& line : MotionLines(clip))
                    out << "  " << line << "\n";
                if (!weapons_.empty())
                {
                    out << "weapon sites (live = recording starts, clip = in the clip, played = by the replay):\n";
                    for (const auto& [site, row] : weapons_)
                        out << "  site=" << Hex(site) << ",live=" << row.live << ",clip=" << row.clip << ",played=" << row.played
                            << "\n";
                }
            }
            out << "basis: " << (rawBasis ? "the recording's own starts (this session recorded the clip)"
                                          : "the clip's content (one-shot events, registered tracks)")
                << "\n\n";
            out << "SUMMARY,effects=" << s.effects << ",rec=" << s.rec << ",play=" << s.play << ",matched=" << s.matched
                << ",missing=" << s.missing << " (" << s.MissingPct() << "%),extra=" << s.extra << " (" << s.ExtraPct()
                << "%)\n";
            out << "PLAY_MAKERS,ours=" << s.ours << ",engine_puppet=" << s.enginePuppet << ",engine_other=" << s.engineOther
                << ",refused_on_puppets=" << s.refused << "\n\n";
            out << "hash        kind  rec  stored shot skip unres  clip | play ours eng_pup eng_other refused | gap  site       name\n";
            std::vector<std::pair<std::uint32_t, Row>> rows(rows_.begin(), rows_.end());
            std::sort(rows.begin(), rows.end(), [rawBasis](const auto& a, const auto& b) {
                const std::uint32_t ra = rawBasis ? a.second.rec : a.second.clip;
                const std::uint32_t rb = rawBasis ? b.second.rec : b.second.clip;
                const std::uint32_t va = std::max(ra, a.second.Play());
                const std::uint32_t vb = std::max(rb, b.second.Play());
                return va != vb ? va > vb : a.first < b.first;
            });
            for (const auto& entry : rows)
            {
                const Row& row = entry.second;
                const std::int64_t rec = rawBasis ? row.rec : row.clip;
                const std::int64_t gap = std::int64_t(row.Play()) - rec;
                if (!rec && !row.Play() && !row.refused)
                    continue;
                char text[256]{};
                std::snprintf(text, sizeof(text), "0x%08x  %-4s %5u %6u %4u %4u %5u %5u | %4u %4u %7u %9u %7u | %+5lld 0x%07x ",
                    entry.first, row.registered ? "reg" : "one", row.rec, row.recStored, row.recShot, row.recSkipped,
                    row.recUnresolved, row.clip, row.Play(), row.playOurs, row.playEnginePuppet, row.playEngineOther,
                    row.refused, static_cast<long long>(gap), row.site);
                out << text;
                const auto name = names.find(entry.first);
                out << (name != names.end() ? name->second : std::string("-")) << "\n";
            }
            out << "\nlegend: rec = starts (one-shot) or creations (registered) while recording; stored/shot/skip/unres = "
                   "where the recording put them; clip = in the clip; play = starts in playback by the replay (ours), the "
                   "engine on a puppet or the hidden player (eng_pup), or the engine elsewhere (eng_other); refused = "
                   "engine starts on puppets refused by the owner rule; gap = play - basis.\n";
            return out.str();
        }

    private:
        void ResetPlayExtras()
        {
            recreates_.clear();
            placementChecked_ = placementFar_ = 0;
            placementMaxM_ = 0.0f;
            impactOnBody_ = 0;
            shotShared_ = shotOnBody_ = 0;
            weaponEvents_ = false;
            shotNoFx_ = shotCulled_ = weaponBonesSeen_ = weaponBonesArmed_ = 0;
            weaponBonesOverwritten_ = weaponBonesMixed_ = weaponBonesShown_ = weaponBonesFrozen_ = 0;
            weaponLookChecked_ = weaponComponentsMismatch_ = weaponWearMismatch_ = 0;
        }

        // The extra columns on the FX_CHECK line.
        void AppendExtras(std::ostream& line) const
        {
            std::uint64_t recreate = 0, reentered = 0, notReady = 0, relPlay = 0, playMotion = 0, movedRec = 0, recMotion = 0,
                          movedPlay = 0, restartsRec = 0, restartsPlay = 0, gapsRec = 0, gapsPlay = 0, dup = 0, late = 0,
                          blinks = 0, settingsDiff = 0;
            for (const auto& entry : rows_)
            {
                const Row& row = entry.second;
                recreate += row.recreateInSpan;
                reentered += row.reentered;
                notReady += row.ownerNotReady;
                relPlay += row.playRelocated;
                playMotion += row.playMotion;
                movedRec += row.recMoved;
                recMotion += row.recMotion;
                movedPlay += row.playMoved;
                restartsRec += row.recRestarts;
                restartsPlay += row.playRestarts;
                gapsRec += row.recGaps;
                gapsPlay += row.playGaps;
                dup += row.dupWorld;
                late += row.ownerLate;
                blinks += row.blinks;
                settingsDiff += row.settingsDiff;
            }
            std::uint32_t weaponLive = 0, weaponClip = 0, weaponPlayed = 0;
            for (const auto& entry : weapons_)
            {
                weaponLive += entry.second.live;
                weaponClip += entry.second.clip;
                weaponPlayed += entry.second.played;
            }
            char pct[96]{};
            std::snprintf(pct, sizeof(pct), ",relocated_play=%.1f,moved_rec=%.1f,moved_play=%.1f", Pct(relPlay, playMotion),
                Pct(movedRec, recMotion), Pct(movedPlay, playMotion));
            line << ",recreate_in_span=" << recreate << ",reentered=" << reentered << ",owner_not_ready=" << notReady << pct
                 << ",restarts_rec=" << restartsRec << ",restarts_play=" << restartsPlay << ",gaps_rec=" << gapsRec
                 << ",gaps_play=" << gapsPlay << ",dup_world=" << dup << ",owner_late=" << late << ",blinks=" << blinks
                 << ",settings_diff=" << settingsDiff << ",weapon_extra=" << (weaponPlayed > weaponClip ? weaponPlayed - weaponClip : 0)
                 << ",weapon_missing=" << (weaponClip > weaponPlayed ? weaponClip - weaponPlayed : 0)
                 << ",weapon_live=" << weaponLive << ",shared=" << shotShared_ << ",on_body=" << (shotOnBody_ + impactOnBody_)
                 << ",placement_far=" << placementFar_ << ",shot_no_fx=" << shotNoFx_ << ",shot_culled=" << shotCulled_
                 << ",weapon_bones_seen=" << weaponBonesSeen_ << ",weapon_bones_armed=" << weaponBonesArmed_
                 << ",weapon_bones_overwritten=" << weaponBonesOverwritten_ << ",weapon_bones_mixed=" << weaponBonesMixed_
                 << ",weapon_bones_shown=" << weaponBonesShown_ << ",weapon_bones_frozen=" << weaponBonesFrozen_
                 << ",weapon_look_checked=" << weaponLookChecked_ << ",weapon_components_mismatch=" << weaponComponentsMismatch_
                 << ",weapon_wear_mismatch=" << weaponWearMismatch_
                 << ",flags=" << Flags().size();
        }

        struct TrackRecreate
        {
            std::uint32_t hash = 0;
            std::uint32_t count = 0;
            std::uint32_t firstMs = 0;
            std::uint32_t lastMs = 0;
            std::uint64_t firstWallMs = 0;
            std::uint64_t lastWallMs = 0;
        };
        std::map<std::size_t, TrackRecreate> recreates_;
        std::map<std::uint32_t, WeaponRow> weapons_;
        std::uint32_t placementChecked_ = 0;
        std::uint32_t placementFar_ = 0;
        float placementMaxM_ = 0.0f;
        std::uint32_t impactOnBody_ = 0;
        std::uint32_t shotShared_ = 0;
        std::uint32_t shotOnBody_ = 0;
        bool weaponEvents_ = false;
        std::uint32_t shotNoFx_ = 0;
        std::uint32_t shotCulled_ = 0;
        std::uint32_t weaponBonesSeen_ = 0;
        std::uint32_t weaponBonesArmed_ = 0;
        std::uint32_t weaponBonesOverwritten_ = 0;
        std::uint32_t weaponBonesMixed_ = 0;
        std::uint32_t weaponBonesShown_ = 0;
        std::uint32_t weaponBonesFrozen_ = 0;
        std::uint32_t weaponLookChecked_ = 0;
        std::uint32_t weaponComponentsMismatch_ = 0;
        std::uint32_t weaponWearMismatch_ = 0;

        static void AppendTop(std::ostringstream& line, const std::vector<std::pair<std::uint32_t, std::int64_t>>& ranked)
        {
            if (ranked.empty())
            {
                line << "none";
                return;
            }
            for (std::size_t n = 0; n < ranked.size() && n < 8; ++n)
                line << (n ? ";" : "") << Hex(ranked[n].first) << ":" << ranked[n].second;
        }

        std::map<std::uint32_t, Row> rows_;
        bool recSeen_ = false;
    };
}
