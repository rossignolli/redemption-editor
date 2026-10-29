#pragma once

// Pure helpers for the pose of a real weapon's own bones (lever, pump,
// cylinder, hammer):
//  - how a pose differs from another (recorded track, drawn buffer);
//  - the verdict of the play check (the drawn weapon against the pose the
//    replay wrote, weapon_bones_overwritten in the fx check).
// The capture places of the weapons in a hand are in object_pose.h.
// No engine, no natives: ReplayCoreTests and the ClipTool use it as is.

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "pose_math.h"

namespace replay::weaponbones
{
    // A bone moved: 1 degree or 1 mm, the threshold of the drawn check of
    // objects (still_pose_core.h) for "same".
    constexpr float kMovedRad = 0.0174533f;
    constexpr float kMovedM = 0.001f;

    inline float QuatAngle(const replay::pose::Quat& a, const replay::pose::Quat& b)
    {
        float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        dot = dot < 0.0f ? -dot : dot;
        const float na = a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w;
        const float nb = b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w;
        if (!(na > 1e-8f) || !(nb > 1e-8f))
            return 0.0f;
        dot /= std::sqrt(na * nb);
        if (dot >= 1.0f)
            return 0.0f;
        return 2.0f * std::acos(dot);
    }

    struct PoseDiff
    {
        std::uint32_t moved = 0;  // bones over kMovedRad or kMovedM
        int worstBone = -1;
        float maxRad = 0.0f;
        float maxM = 0.0f;
    };

    // Locals of two poses of the same skeleton. Bone 0 (the weapon root, set
    // by the attach) is left out: only the weapon's own parts count.
    inline PoseDiff DiffLocals(const replay::pose::BoneQts* a, const replay::pose::BoneQts* b, std::size_t bones)
    {
        PoseDiff diff;
        if (!a || !b)
            return diff;
        float worst = 0.0f;
        for (std::size_t n = 1; n < bones; ++n)
        {
            const float rad = QuatAngle(a[n].rotation, b[n].rotation);
            const float dx = a[n].translation[0] - b[n].translation[0];
            const float dy = a[n].translation[1] - b[n].translation[1];
            const float dz = a[n].translation[2] - b[n].translation[2];
            const float m = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!(rad == rad) || !(m == m))
                continue; // NaN in a buffer read mid-write: not a verdict
            if (rad > diff.maxRad)
                diff.maxRad = rad;
            if (m > diff.maxM)
                diff.maxM = m;
            if (rad > kMovedRad || m > kMovedM)
            {
                ++diff.moved;
                // Worst by an angle-equivalent score (1 cm ~ 1 degree).
                const float score = rad / kMovedRad + m / (10.0f * kMovedM);
                if (score > worst)
                {
                    worst = score;
                    diff.worstBone = static_cast<int>(n);
                }
            }
        }
        return diff;
    }

    // ---- play check -------------------------------------------------------

    // Per armed weapon, at the start of a script tick (before this tick's
    // write): the drawn buffer against the pose written last tick (did the
    // engine overwrite it?) and the recorded track's motion around the
    // playhead (was there anything to show?).
    struct PlayCheck
    {
        std::uint32_t checks = 0;       // ticks with a drawn read
        std::uint32_t trackMoving = 0;  // of those, ticks whose recorded pose moved since the last check
        std::uint32_t drawnSame = 0;    // drawn == written last tick
        std::uint32_t drawnOther = 0;   // drawn != written: the engine wrote after us
        std::uint32_t drawnMovingOther = 0; // drawnOther while the track moved (the lost motion)
        float maxRad = 0.0f;            // worst drawn-vs-written angle
        int worstBone = -1;
    };

    inline void NoteCheck(PlayCheck& check, bool trackMoved, const PoseDiff& drawnVsWritten)
    {
        ++check.checks;
        check.trackMoving += trackMoved ? 1u : 0u;
        if (drawnVsWritten.moved == 0)
        {
            ++check.drawnSame;
            return;
        }
        ++check.drawnOther;
        check.drawnMovingOther += trackMoved ? 1u : 0u;
        if (drawnVsWritten.maxRad > check.maxRad)
        {
            check.maxRad = drawnVsWritten.maxRad;
            check.worstBone = drawnVsWritten.worstBone;
        }
    }

    enum class PlayVerdict : std::uint8_t
    {
        None,        // too few checks, or the track never moved in them
        Shown,       // the recorded motion is what was drawn
        Overwritten, // the engine drew something else in most moving ticks
        Mixed,
    };

    constexpr std::uint32_t kMinMovingChecks = 5;

    inline PlayVerdict Verdict(const PlayCheck& check)
    {
        if (check.trackMoving < kMinMovingChecks)
            return PlayVerdict::None;
        // Ticks whose recorded pose moved and whose drawn pose was ours.
        const std::uint32_t lost = check.drawnMovingOther;
        if (lost * 2u >= check.trackMoving)
            return PlayVerdict::Overwritten;
        if (lost * 10u <= check.trackMoving)
            return PlayVerdict::Shown;
        return PlayVerdict::Mixed;
    }

    // Per bone, the ticks whose recorded pose turned that bone (since the tick
    // before) against the ticks the drawn buffer turned it. A bone the record turns
    // and the drawing does not is a frozen part (a revolver's cylinder), even with
    // the whole-pose verdict "shown".
    constexpr std::uint32_t kFrozenMinRecTicks = 3;

    inline bool FrozenBone(std::uint32_t recTicks, std::uint32_t drawnTicks)
    {
        return recTicks >= kFrozenMinRecTicks && drawnTicks * 4u < recTicks;
    }

    inline const char* VerdictName(PlayVerdict verdict)
    {
        switch (verdict)
        {
        case PlayVerdict::Shown:
            return "shown";
        case PlayVerdict::Overwritten:
            return "overwritten";
        case PlayVerdict::Mixed:
            return "mixed";
        default:
            return "none";
        }
    }
}
