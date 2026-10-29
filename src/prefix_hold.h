#pragma once

// A body puppet posed through a skeleton that shrank to a bone count no
// record has, for the frames until a record fits it again.
//
// A head shot that blows the head off (gore) makes the engine rebuild the
// ped's skeleton in place: same pointer, the head's layers gone, then the
// stump's layers on. Example: 998 bones (the record before the shot), 705 for
// two or three frames (the body group alone), then 857 (the record after
// it). No record has 705 bones, so the segment
// switch released the arm (segment_switch.h: no_record_for_bones), the detour
// wrote nothing (bone_mismatch: the bind pose) and the unposed hide hid the
// body until the 857 record armed: the puppet blinked at the shot.
//
// The rule, as a replay writes only the bones the skeleton and the frame
// share: while the
// skeleton has fewer bones than the armed record and no more than the
// record's body group (the first group of its layout, which every bone of a
// smaller rebuild of the same skeleton keeps in order), the armed pose's first
// bones (locals and objects) are written into it. The arm is held, so the
// body stays shown and posed; the alternate pose and the record of the next
// segment take over as soon as the engine reaches their bone count. The
// puppet's parent table is read on the next frame and compared with the
// record's first bones: a mismatch, a table that cannot be read or a hold
// past kMaxMs gives the old path back (release, the skeleton map).
//
// Pure: unit tested in ReplayCoreTests (appearance_tests.cpp).

#include "pose_skeleton_layout.h"
#include "skeleton_map.h"

#include <cstdint>
#include <vector>

namespace replay::prefixhold
{
    // The hold's bound: a rebuild in progress lasts a few frames; a skeleton
    // that stays at a count no record has goes to the skeleton map.
    constexpr std::uint64_t kMaxMs = 600;

    // The bones of the record layout's body group (the group of bone 0, split
    // as the skeleton map splits it), from the layout's raw parents. 0 when
    // there is no layout.
    inline std::uint32_t BodyBones(const std::int32_t* parents, std::uint32_t count)
    {
        if (!parents || count == 0)
            return 0;
        std::vector<std::int32_t> normalised;
        pose::NormaliseParents(parents, count, normalised);
        std::vector<pose::SkeletonGroup> groups;
        pose::SplitSkeletonGroups(normalised, groups);
        return groups.empty() || groups[0].first != 0 ? 0u : groups[0].count;
    }

    // The skeleton's `liveBones` take the first bones of the armed pose: fewer
    // than the armed record's, within its body group.
    inline bool Writable(std::uint32_t liveBones, std::uint32_t armedBones, std::uint32_t bodyBones)
    {
        return bodyBones != 0 && liveBones != 0 && liveBones < armedBones && liveBones <= bodyBones;
    }

    // The puppet's engine parent table against the record's first bones: each
    // bone's parent the same (bone 0 is the root on both sides; a record bone
    // kept in object space, kUnresolvedParent, vouches for nothing either way).
    inline bool SameFront(const std::int32_t* recordParents, std::uint32_t recordCount,
        const std::uint16_t* cloneParents, std::uint32_t cloneCount)
    {
        if (!recordParents || !cloneParents || cloneCount == 0 || cloneCount > recordCount)
            return false;
        for (std::uint32_t bone = 1; bone < cloneCount; ++bone)
        {
            const std::int32_t recorded = recordParents[bone];
            if (recorded == pose::kUnresolvedParent)
                continue;
            const std::int32_t clone = cloneParents[bone] < bone ? static_cast<std::int32_t>(cloneParents[bone]) : -1;
            const std::int32_t want = recorded >= 0 && static_cast<std::uint32_t>(recorded) < bone ? recorded : -1;
            if (clone != want)
                return false;
        }
        return true;
    }

    enum class Verdict : std::uint8_t
    {
        Pending,  // the table not read yet
        Same,     // the front matches: held until a record fits
        Differ,   // released (the old path)
        Unread,   // the table could not be read: released
    };

    inline const char* VerdictText(Verdict verdict)
    {
        switch (verdict)
        {
        case Verdict::Same:
            return "same";
        case Verdict::Differ:
            return "differ";
        case Verdict::Unread:
            return "unread";
        default:
            return "pending";
        }
    }

    // One hold per body: begun on the tick the shrunk skeleton is seen, ended
    // when a record fits it again, the skeleton grows back, or it is released.
    struct Hold
    {
        std::uint64_t since = 0;
        std::uint32_t bones = 0;   // the skeleton's bone count held
        std::uint32_t ticks = 0;   // ticks held
        Verdict verdict = Verdict::Pending;

        bool Active() const { return since != 0; }

        // True when this tick begins a hold (or the skeleton changed count
        // again while held: compared again).
        bool Note(std::uint32_t liveBones, std::uint64_t nowMs)
        {
            const bool begins = !Active() || bones != liveBones;
            if (begins)
            {
                since = nowMs ? nowMs : 1;
                bones = liveBones;
                ticks = 0;
                verdict = Verdict::Pending;
            }
            ++ticks;
            return begins;
        }

        bool Expired(std::uint64_t nowMs) const { return Active() && nowMs > since && nowMs - since > kMaxMs; }

        // The hold goes on this tick: verified, or not read yet and in time.
        bool Holds(std::uint64_t nowMs) const
        {
            return Active() && !Expired(nowMs) && (verdict == Verdict::Same || verdict == Verdict::Pending);
        }

        void End()
        {
            since = 0;
            bones = 0;
            ticks = 0;
            verdict = Verdict::Pending;
        }
    };
}
