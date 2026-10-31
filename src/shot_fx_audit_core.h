#pragma once

// The weapon effects of the shot track, audited and (clips recorded before
// the weapon effects were events) de-duplicated.
//
// Older clips (before the weapon effect events) do not keep the muzzle flash and the bullet
// impact as effects: each shot took the nearest trigger of the muzzle site
// (weapon_fx_intent_core.h kWeaponMuzzleCall) and of the impact site
// (kShotImpactCall) within its window
// (FlushPoseV2FxWindows) and the playback makes them again with scale 1 and
// no engine state. In a gunfight two shots of different shooters a few ms
// apart can claim the same trigger, so one flash or impact plays twice and
// the other shooter's is lost; a shot that hit a body also gets the nearest
// surface impact. Newer clips record one event per effect start
// (the weapon effect events), never a match by time, and the shot keeps none. For
// older clips the audit counts the shared claims and the legacy rule plays a
// shared hash once.
//
// Pure (no game); unit tested in tests/particle_replay_tests.cpp.

#include "game_sig_core.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace replay::shotfx
{
    // The trigger sites of a shot's effects other than the muzzle flash (the
    // call of the triggered instance wrapper; the recorded site is its return
    // address, fx_observer.h BulletImpactSite and BodyHitSite): the bullet
    // impact on a surface, and the blood/body hit effect of a bullet (a
    // one-shot track).
    inline const sig::Signature kShotImpactCore{"fx_bullet_impact_core",
        "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 18 FF FF FF 48 81 EC C0 01 "
        "00 00 4C 8B BD 10 01 00 00",
        0x144D0CC};
    inline const sig::Signature kShotImpactCall{"fx_bullet_impact_call", kShotImpactCore, sig::Offset{0x269},
        "E8 ?? ?? ?? ?? 48 8B D8 48 85 C0", 0x144D335};
    inline const sig::Signature kShotBodyHit{"fx_body_hit_core",
        "48 8B C4 48 89 58 10 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 90 48 81 EC 70 01 00 00",
        0x144BF9C};
    inline const sig::Signature kShotBodyCall{"fx_body_hit_call", kShotBodyHit, sig::Offset{0xD5},
        "E8 ?? ?? ?? ?? 4C 8B F0 48 85 C0", 0x144C071};
    constexpr std::uint32_t kSharedWindowMs = 100;    // two shots this close claimed the same trigger
    constexpr std::int32_t kBodyBeforeMs = 30;        // a body effect within [-30, +10] ms of the shot: it hit a body
    constexpr std::int32_t kBodyAfterMs = 10;

    struct Shot
    {
        std::uint32_t timeMs = 0;
        std::uint32_t shooter = 0; // packed id
        bool player = false;       // the recorded player (ShotEvent::shooterPlayer == 2)
        std::uint32_t muzzleFx = 0;
        std::uint32_t impactFx = 0;
        bool hasImpact = false;
    };

    struct Decision
    {
        bool playMuzzle = true;
        bool playImpact = true;
        bool sharedMuzzle = false;
        bool sharedImpact = false;
        bool onBody = false;
        bool pellet = false; // another shot of the same shooter at the same ms (a shotgun's pellets)
    };

    struct Audit
    {
        std::uint32_t shots = 0;
        std::uint32_t muzzle = 0;
        std::uint32_t muzzleShared = 0;
        std::uint32_t impact = 0;
        std::uint32_t impactShared = 0;
        std::uint32_t impactOnBody = 0;
        std::uint32_t pelletShots = 0;
        std::uint32_t legacySkippedMuzzle = 0;
        std::uint32_t legacySkippedImpact = 0;
        bool weaponEvents = false; // the clip has the weapon effects as events (the weapon effect events)

        const char* Verdict() const
        {
            if (weaponEvents)
                return "events";
            return (muzzleShared || impactShared || impactOnBody) ? "legacy_shared" : "legacy_ok";
        }

        std::string Line(const std::string& clip) const
        {
            std::ostringstream line;
            line << "FX_SHOT_AUDIT,clip=" << clip << ",shots=" << shots << ",muzzle=" << muzzle
                 << ",muzzle_shared=" << muzzleShared << ",impact=" << impact << ",impact_shared=" << impactShared
                 << ",impact_on_body=" << impactOnBody << ",pellet_shots=" << pelletShots
                 << ",weapon_events=" << (weaponEvents ? 1 : 0) << ",legacy_skipped_muzzle=" << legacySkippedMuzzle
                 << ",legacy_skipped_impact=" << legacySkippedImpact << ",verdict=" << Verdict();
            return line.str();
        }
    };

    inline std::uint32_t AbsDiff(std::uint32_t a, std::uint32_t b) { return a > b ? a - b : b - a; }

    // shots: time ascending. bodyTimes: the times of the clip's body hit
    // effects (site kShotBodyCall). dedupe: the legacy rule (always on for an older clip).
    // A shared hash is played once: by the player's shot when one of the
    // group is the player's, else by the first; a surface impact is not
    // played for a shot with a body hit effect in [-30, +10] ms.
    inline std::vector<Decision> Decide(const std::vector<Shot>& shots, const std::vector<std::uint32_t>& bodyTimes,
        bool dedupe, Audit& audit)
    {
        std::vector<Decision> out(shots.size());
        audit.shots = static_cast<std::uint32_t>(shots.size());
        for (std::size_t n = 0; n < shots.size(); ++n)
        {
            const Shot& shot = shots[n];
            Decision& d = out[n];
            if (shot.muzzleFx)
                ++audit.muzzle;
            if (shot.impactFx && shot.hasImpact)
                ++audit.impact;
            for (std::size_t m = 0; m < shots.size(); ++m)
            {
                if (m == n)
                    continue;
                const Shot& other = shots[m];
                if (AbsDiff(other.timeMs, shot.timeMs) > kSharedWindowMs)
                    continue;
                if (other.shooter == shot.shooter)
                {
                    if (other.timeMs == shot.timeMs)
                        d.pellet = true;
                    continue;
                }
                if (shot.muzzleFx && other.muzzleFx == shot.muzzleFx)
                    d.sharedMuzzle = true;
                if (shot.impactFx && shot.hasImpact && other.impactFx == shot.impactFx && other.hasImpact)
                    d.sharedImpact = true;
            }
            for (const std::uint32_t body : bodyTimes)
            {
                const std::int64_t dt = static_cast<std::int64_t>(body) - static_cast<std::int64_t>(shot.timeMs);
                if (dt >= -kBodyBeforeMs && dt <= kBodyAfterMs)
                    d.onBody = true;
            }
            audit.muzzleShared += d.sharedMuzzle ? 1 : 0;
            audit.impactShared += d.sharedImpact ? 1 : 0;
            audit.impactOnBody += (d.onBody && shot.impactFx && shot.hasImpact) ? 1 : 0;
            audit.pelletShots += d.pellet ? 1 : 0;
        }
        if (!dedupe)
            return out;
        // The keeper of each shared group: the player's shot, else the first.
        const auto keeper = [&](std::size_t n, bool muzzle) {
            const Shot& shot = shots[n];
            const std::uint32_t hash = muzzle ? shot.muzzleFx : shot.impactFx;
            std::size_t best = n;
            for (std::size_t m = 0; m < shots.size(); ++m)
            {
                const Shot& other = shots[m];
                if (other.shooter == shot.shooter && m != n)
                    continue;
                if (AbsDiff(other.timeMs, shot.timeMs) > kSharedWindowMs)
                    continue;
                if ((muzzle ? other.muzzleFx : other.impactFx) != hash)
                    continue;
                const Shot& held = shots[best];
                if ((other.player && !held.player) || (other.player == held.player && m < best))
                    best = m;
            }
            return best;
        };
        for (std::size_t n = 0; n < shots.size(); ++n)
        {
            Decision& d = out[n];
            if (d.sharedMuzzle && keeper(n, true) != n)
            {
                d.playMuzzle = false;
                ++audit.legacySkippedMuzzle;
            }
            bool skipImpact = d.sharedImpact && keeper(n, false) != n;
            if (d.onBody && shots[n].impactFx && shots[n].hasImpact)
                skipImpact = true;
            if (skipImpact)
            {
                d.playImpact = false;
                ++audit.legacySkippedImpact;
            }
        }
        return out;
    }
}
