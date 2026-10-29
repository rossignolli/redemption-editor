#pragma once

// Attached objects with a skeleton that never animates (a pelt carried on a
// horse). Pure part, unit tested in ReplayCoreTests.
//
// 1. The still rule (ini StillPoseEngine, default 1). An attached object
//    with a skeleton whose pose record never changed over the clip
//    (tpose::NeverAnimated on the clip's track: no pose entry after the
//    first frame over at least 60 frames and 2 s; at record time the same
//    objects log detour_calls=0 and never_animated=1) is not pose-armed on
//    its replay object: the engine poses the clone attached to the same
//    bone, as it posed the original live. Arming it instead writes its
//    constant rest pose every tick; the engine overwrites nearly all of
//    those writes and the image flickers.
// 2. The drawn check (ini ObjectDrawnCheck, default 0, QA package 1). Each
//    tick, before the publish, the locals the last frame drew against the
//    pose the replay expects (armed: the published one; still rule: the
//    record's constant pose) and against the model's bind pose: Same, Bind
//    (the engine drew its bind pose over ours: a flat sheet)
//    or Other (the engine's own pose: attach or fragment physics). Flips
//    between Same and a changed shape are the flicker, counted per puppet.

#include "pose_math.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace replay::stillpose
{
    // ---- 1. the still rule ----

    enum class Decision : std::uint8_t
    {
        Arm,          // armed: the record animates
        SkipStill,    // not armed: attached and never animated
        ArmRuleOff,   // StillPoseEngine=0
        ArmFree,      // not attached to a parent (a dropped pelt, a world object)
        ArmUnread,    // the clip's track could not be read: armed
    };

    inline Decision Decide(bool ruleOn, bool attachedToParent, bool signatureParsed, bool neverAnimated)
    {
        if (!signatureParsed)
            return Decision::ArmUnread;
        if (!neverAnimated)
            return Decision::Arm;
        if (!ruleOn)
            return Decision::ArmRuleOff;
        if (!attachedToParent)
            return Decision::ArmFree;
        return Decision::SkipStill;
    }

    inline const char* DecisionName(Decision decision)
    {
        switch (decision)
        {
        case Decision::Arm: return "arm";
        case Decision::SkipStill: return "still_engine_posed";
        case Decision::ArmRuleOff: return "still_armed_rule_off";
        case Decision::ArmFree: return "still_armed_free";
        case Decision::ArmUnread: return "arm_track_unread";
        }
        return "?";
    }

    // ---- 2. the drawn check ----

    // A drawn bone is the expected one within a degree and 5 mm (the stream's
    // error is a fraction of that); it is the bind pose within 5 degrees
    // (tpose::kPosedRad, the same threshold the T-pose verdict uses).
    constexpr float kSameRad = 0.0175f;
    constexpr float kSameM = 0.005f;
    constexpr float kBindRad = 0.0873f;

    enum class Shape : std::uint8_t
    {
        Same,  // drawn = expected
        Bind,  // every changed bone at its bind rotation
        Other, // the engine's own pose (or no bind to tell)
        Objects, // locals as expected, object matrices not (the engine
                 // composed them again: what the mesh is skinned with)
    };

    inline const char* ShapeName(Shape shape)
    {
        switch (shape)
        {
        case Shape::Same: return "same";
        case Shape::Bind: return "bind";
        case Shape::Other: return "other";
        case Shape::Objects: return "objects";
        }
        return "?";
    }

    // Angle between two rotations (0 to pi), -1 when either is unusable.
    inline float Angle(const pose::Quat& a, const pose::Quat& b)
    {
        const float na = a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w;
        const float nb = b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w;
        if (!(na > 0.25f && na < 4.0f) || !(nb > 0.25f && nb < 4.0f))
            return -1.0f;
        float dot = std::fabs((a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) / std::sqrt(na * nb));
        dot = dot > 1.0f ? 1.0f : dot;
        return 2.0f * std::acos(dot);
    }

    struct Comparison
    {
        std::uint32_t bones = 0;
        std::uint32_t changed = 0;  // bones off the expected pose
        std::uint32_t nearBind = 0; // of those, at their bind rotation
        std::int32_t worstBone = -1;
        float maxRad = 0.0f;
        float maxM = 0.0f;
        Shape shape = Shape::Same;
    };

    // `bind`: the bind rotation per bone, or null (unknown: a changed pose is
    // Other). A bone with an unusable rotation on either side counts as
    // changed (a torn read or a rebuilt skeleton is not Same).
    inline Comparison Compare(const pose::BoneQts* expected, const pose::BoneQts* drawn, const pose::Quat* bind,
        std::uint32_t bones)
    {
        Comparison c;
        c.bones = bones;
        float worst = -1.0f;
        for (std::uint32_t bone = 0; bone < bones; ++bone)
        {
            const float rad = Angle(expected[bone].rotation, drawn[bone].rotation);
            const float dx = expected[bone].translation[0] - drawn[bone].translation[0];
            const float dy = expected[bone].translation[1] - drawn[bone].translation[1];
            const float dz = expected[bone].translation[2] - drawn[bone].translation[2];
            const float m = std::sqrt(dx * dx + dy * dy + dz * dz);
            const bool unusable = rad < 0.0f || !(m == m);
            if (!unusable && rad <= kSameRad && m <= kSameM)
                continue;
            ++c.changed;
            const float r = unusable ? 4.0f : rad;
            if (r > c.maxRad)
                c.maxRad = r;
            if (!unusable && m > c.maxM)
                c.maxM = m;
            const float score = r + m;
            if (score > worst)
            {
                worst = score;
                c.worstBone = static_cast<std::int32_t>(bone);
            }
            if (bind && !unusable)
            {
                const float toBind = Angle(drawn[bone].rotation, bind[bone]);
                if (toBind >= 0.0f && toBind <= kBindRad)
                    ++c.nearBind;
            }
        }
        if (c.changed == 0)
            c.shape = Shape::Same;
        else if (bind && c.nearBind == c.changed)
            c.shape = Shape::Bind;
        else
            c.shape = Shape::Other;
        return c;
    }

    // Locals then object matrices (the engine buffer's two halves): the
    // locals decide the shape; when they match, object matrices that differ
    // make it Objects. objChanged/objMaxRad/objMaxM describe the second half.
    struct FullComparison
    {
        Comparison locals;
        Comparison objects;
        Shape shape = Shape::Same;
        float maxRad = 0.0f;
        float maxM = 0.0f;
    };

    inline FullComparison CompareFull(const pose::BoneQts* expected, const pose::BoneQts* drawn, const pose::Quat* bind,
        std::uint32_t bones, bool withObjects)
    {
        FullComparison f;
        f.locals = Compare(expected, drawn, bind, bones);
        if (withObjects)
            f.objects = Compare(expected + bones, drawn + bones, nullptr, bones);
        f.shape = f.locals.shape != Shape::Same ? f.locals.shape
            : (withObjects && f.objects.changed ? Shape::Objects : Shape::Same);
        f.maxRad = f.locals.maxRad > f.objects.maxRad ? f.locals.maxRad : f.objects.maxRad;
        f.maxM = f.locals.maxM > f.objects.maxM ? f.locals.maxM : f.objects.maxM;
        return f;
    }

    // Per puppet: how many ticks were checked and drawn which shape, how
    // often the drawn pose flipped between the expected one and another (the
    // flicker), and the longest run of changed ticks.
    struct Totals
    {
        std::uint64_t checks = 0;
        std::uint64_t same = 0;
        std::uint64_t bind = 0;
        std::uint64_t other = 0;
        std::uint64_t objects = 0;
        std::uint64_t flips = 0;
        std::uint64_t run = 0;
        std::uint64_t longestRun = 0;
        float maxRad = 0.0f;
        float maxM = 0.0f;
        bool hasLast = false;
        bool lastChanged = false;

        // True when this tick flipped (first tick: false).
        bool Add(const Comparison& c) { return Add(c.shape, c.maxRad, c.maxM); }
        bool Add(const FullComparison& f) { return Add(f.shape, f.maxRad, f.maxM); }

        bool Add(Shape shape, float rad, float m)
        {
            ++checks;
            const bool changed = shape != Shape::Same;
            if (shape == Shape::Same)
                ++same;
            else if (shape == Shape::Bind)
                ++bind;
            else if (shape == Shape::Objects)
                ++objects;
            else
                ++other;
            if (rad > maxRad)
                maxRad = rad;
            if (m > maxM)
                maxM = m;
            run = changed ? run + 1 : 0;
            if (run > longestRun)
                longestRun = run;
            const bool flipped = hasLast && changed != lastChanged;
            if (flipped)
                ++flips;
            hasLast = true;
            lastChanged = changed;
            return flipped;
        }

        std::uint64_t Changed() const { return bind + other + objects; }
    };

    // The verdict of one puppet's totals: "clean" (always the expected pose),
    // "flicker" (it flips: more than one flip per 30 checks, a second at the
    // capture rate), "replaced" (changed most of the time without flipping:
    // the engine's pose shows steadily), "mixed" otherwise, "none" unchecked.
    inline const char* Verdict(const Totals& t)
    {
        if (t.checks == 0)
            return "none";
        if (t.Changed() == 0)
            return "clean";
        if (t.flips * 30 > t.checks)
            return "flicker";
        if (t.Changed() * 10 >= t.checks * 9)
            return "replaced";
        return "mixed";
    }

    inline std::string FormatTotals(const Totals& t)
    {
        char text[288];
        std::snprintf(text, sizeof(text),
            "checks=%llu,same=%llu,bind=%llu,other=%llu,objects=%llu,flips=%llu,longest_changed_run=%llu,max_rad=%.3f,"
            "max_m=%.3f,verdict=%s",
            static_cast<unsigned long long>(t.checks), static_cast<unsigned long long>(t.same),
            static_cast<unsigned long long>(t.bind), static_cast<unsigned long long>(t.other),
            static_cast<unsigned long long>(t.objects),
            static_cast<unsigned long long>(t.flips), static_cast<unsigned long long>(t.longestRun), t.maxRad, t.maxM,
            Verdict(t));
        return text;
    }

    // Lines of POSE_V2_OBJECT_DRAWN per playback: the first check of each
    // puppet and every flip while under the cap.
    constexpr std::uint32_t kDrawnLineCap = 96;
    inline bool ShouldLogCheck(bool first, bool flipped, std::uint32_t linesSoFar)
    {
        return (first || flipped) && linesSoFar < kDrawnLineCap;
    }
}
