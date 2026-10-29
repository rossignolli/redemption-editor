#pragma once

// The pose of a record on a puppet whose skeleton has another bone count.
//
// The engine builds a ped or horse skeleton from groups (the body, then one
// per attached piece: tack, a horse's extra chain); a horse puppet can come
// with a group more or a different tack than the recording (e.g. 705 bones
// recorded, 710 built), and a pose that requires an exact bone count would
// leave it in its bind pose. As a replay editor usually does, the pose is
// applied to the bones both sides share; here per group, from the two parent
// tables:
//
// - Each table is split into groups: a group starts at bone 0, at a bone
//   whose parent is not an ancestor of the bone before it (the depth-first
//   order breaks: a new piece hangs off an earlier bone), or at a bone whose
//   parent lies before the current group. On the horses tested this gives
//   exactly the engine's group count (4 or 5).
// - Puppet groups are matched in order with the first unused recorded
//   group of the same size, the same internal parents, and a root hanging
//   off the same (already mapped) bone. The two body groups (bone 0's) may
//   differ in size: a piece hung off the body's last bone reads as part of
//   it, so their shared front is matched and the rest of each becomes a
//   group of its own, matched like the others. Matched bones take the recorded
//   local and object; puppet-only bones keep their own local (the rest pose
//   the engine gave them) and get their object composed from their parent's,
//   as the finalizer composes it; recorded-only bones are dropped.
// - Refused when the group with bone 0 does not match or less than half of
//   the puppet's bones would be posed: an unrelated skeleton never gets a
//   pose from another rig.
//
// Pure: no engine access; unit tested in ReplayCoreTests.

#include "pose_math.h"

#include <cstdint>
#include <vector>

namespace replay::pose
{
    struct SkeletonGroup
    {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        std::int32_t rootParent = -1; // the root's parent (-1: a root of the skeleton)
    };

    // A parent table normalised: a parent that is negative or not an
    // earlier bone means "no parent" (-1). The recorded layout marks bone 0
    // -2 (object space); the engine table gives bone 0 whatever it holds.
    inline void NormaliseParents(const std::int32_t* parents, std::uint32_t count, std::vector<std::int32_t>& out)
    {
        out.resize(count);
        for (std::uint32_t bone = 0; bone < count; ++bone)
        {
            const std::int32_t parent = parents[bone];
            out[bone] = parent >= 0 && static_cast<std::uint32_t>(parent) < bone ? parent : -1;
        }
    }

    // `parents` normalised (NormaliseParents).
    inline void SplitSkeletonGroups(const std::vector<std::int32_t>& parents, std::vector<SkeletonGroup>& groups)
    {
        groups.clear();
        const std::uint32_t count = static_cast<std::uint32_t>(parents.size());
        for (std::uint32_t bone = 0; bone < count; ++bone)
        {
            const std::int32_t parent = parents[bone];
            bool starts = bone == 0 || parent < 0 || groups.empty() ||
                static_cast<std::uint32_t>(parent) < groups.back().first;
            if (!starts)
            {
                // The parent must be the bone before or one of its ancestors.
                std::int32_t walk = static_cast<std::int32_t>(bone) - 1;
                while (walk >= 0 && walk != parent)
                    walk = parents[static_cast<std::uint32_t>(walk)];
                starts = walk != parent;
            }
            if (starts)
                groups.push_back({bone, 1u, parent});
            else
                ++groups.back().count;
        }
    }

    enum class SkeletonMapMode : std::uint8_t
    {
        Exact = 0,   // same table: bone n is bone n
        Mapped = 1,  // per group
        Refused = 2,
    };

    inline const char* SkeletonMapModeText(SkeletonMapMode mode)
    {
        switch (mode)
        {
        case SkeletonMapMode::Exact:
            return "exact";
        case SkeletonMapMode::Mapped:
            return "mapped";
        default:
            return "refused";
        }
    }

    struct SkeletonMap
    {
        SkeletonMapMode mode = SkeletonMapMode::Refused;
        const char* refuseReason = "none";
        std::uint32_t sourceBones = 0; // recorded
        std::uint32_t targetBones = 0; // puppet
        // Per puppet bone: the recorded bone it takes, or -1 (rest pose).
        std::vector<std::int32_t> source;
        // The puppet's normalised parent table (composes the rest bones).
        std::vector<std::int32_t> targetParents;
        std::uint32_t matched = 0; // puppet bones posed from the record
        std::uint32_t rest = 0;    // puppet bones in their rest pose
        std::uint32_t dropped = 0; // recorded bones with no puppet bone
        std::uint32_t sourceGroups = 0;
        std::uint32_t targetGroups = 0;
        std::uint32_t matchedGroups = 0;
    };

    // `sourceParents`: the record's layout parents; `targetParents`: the
    // puppet's engine parent table. Both raw (normalised here).
    inline SkeletonMap BuildSkeletonMap(
        const std::int32_t* sourceParents, std::uint32_t sourceCount,
        const std::int32_t* targetParents, std::uint32_t targetCount)
    {
        SkeletonMap map;
        map.sourceBones = sourceCount;
        map.targetBones = targetCount;
        if (!sourceParents || !targetParents || sourceCount == 0 || targetCount == 0)
        {
            map.refuseReason = "empty";
            map.dropped = sourceCount;
            map.rest = targetCount;
            return map;
        }
        std::vector<std::int32_t> source;
        NormaliseParents(sourceParents, sourceCount, source);
        NormaliseParents(targetParents, targetCount, map.targetParents);
        std::vector<SkeletonGroup> sourceGroups;
        std::vector<SkeletonGroup> targetGroups;
        SplitSkeletonGroups(source, sourceGroups);
        SplitSkeletonGroups(map.targetParents, targetGroups);
        map.sourceGroups = static_cast<std::uint32_t>(sourceGroups.size());
        map.targetGroups = static_cast<std::uint32_t>(targetGroups.size());
        map.source.assign(targetCount, -1);
        // Whether `n` bones of target group t take source group g: each
        // parent inside its group at the same offset on both sides, or
        // outside on both sides and then the recorded bone already mapped
        // onto the puppet's parent (both skeleton roots also agree).
        const auto fits = [&](const SkeletonGroup& t, const SkeletonGroup& g, std::uint32_t n) {
            for (std::uint32_t k = 0; k < n; ++k)
            {
                const std::int32_t pt = map.targetParents[t.first + k];
                const std::int32_t ps = source[g.first + k];
                const bool tIn = pt >= static_cast<std::int32_t>(t.first);
                const bool sIn = ps >= static_cast<std::int32_t>(g.first);
                if (tIn != sIn)
                    return false;
                if (tIn)
                {
                    if (pt - static_cast<std::int32_t>(t.first) != ps - static_cast<std::int32_t>(g.first))
                        return false;
                }
                else if (pt < 0 || ps < 0)
                {
                    if (pt != ps)
                        return false;
                }
                else if (map.source[static_cast<std::uint32_t>(pt)] != ps)
                {
                    return false;
                }
            }
            return true;
        };
        const auto take = [&](const SkeletonGroup& t, const SkeletonGroup& g, std::uint32_t n) {
            for (std::uint32_t k = 0; k < n; ++k)
                map.source[t.first + k] = static_cast<std::int32_t>(g.first + k);
            map.matched += n;
            ++map.matchedGroups;
        };
        std::vector<char> used(sourceGroups.size(), 0);
        bool rootGroupMatched = false;
        // Puppet groups in bone order (a parent is always matched or not
        // before its children's group is looked at).
        for (std::size_t ti = 0; ti < targetGroups.size(); ++ti)
        {
            const SkeletonGroup target = targetGroups[ti];
            bool done = false;
            for (std::size_t gi = 0; gi < sourceGroups.size() && !done; ++gi)
            {
                if (used[gi] || sourceGroups[gi].count != target.count || !fits(target, sourceGroups[gi], target.count))
                    continue;
                used[gi] = 1;
                take(target, sourceGroups[gi], target.count);
                done = true;
            }
            if (!done && target.first == 0)
            {
                // The body groups of both sides, of different sizes: a piece
                // hanging off the body's last bone continues its depth-first
                // order and reads as part of the body. The shared front is
                // matched and each side's rest becomes a group of its own,
                // looked at like the others.
                for (std::size_t gi = 0; gi < sourceGroups.size(); ++gi)
                {
                    const SkeletonGroup body = sourceGroups[gi];
                    if (used[gi] || body.first != 0)
                        continue;
                    const std::uint32_t n = body.count < target.count ? body.count : target.count;
                    if (!fits(target, body, n))
                        break;
                    used[gi] = 1;
                    take(target, body, n);
                    done = true;
                    if (target.count > n)
                        targetGroups.insert(targetGroups.begin() + static_cast<std::ptrdiff_t>(ti + 1),
                            SkeletonGroup{target.first + n, target.count - n, map.targetParents[target.first + n]});
                    if (body.count > n)
                    {
                        sourceGroups.push_back({body.first + n, body.count - n, source[body.first + n]});
                        used.push_back(0);
                    }
                    break;
                }
            }
            if (done && target.first == 0)
                rootGroupMatched = true;
        }
        map.rest = targetCount - map.matched;
        map.dropped = sourceCount - map.matched;
        if (!rootGroupMatched)
            map.refuseReason = "root_group";
        else if (map.matched * 2u < targetCount)
            map.refuseReason = "too_few_bones";
        else
        {
            bool identity = sourceCount == targetCount;
            for (std::uint32_t bone = 0; identity && bone < targetCount; ++bone)
                identity = map.source[bone] == static_cast<std::int32_t>(bone);
            map.mode = identity ? SkeletonMapMode::Exact : SkeletonMapMode::Mapped;
            map.refuseReason = "none";
        }
        return map;
    }

    // The puppet's pose from a recorded one through `map` (not refused).
    // `restLocals`: the puppet's own locals, targetBones of them. Every
    // array but the recorded ones holds targetBones entries.
    inline bool ApplySkeletonMap(const SkeletonMap& map,
        const BoneQts* recordedLocals, const BoneQts* recordedObjects, std::uint32_t recordedBones,
        const BoneQts* restLocals, BoneQts* locals, BoneQts* objects)
    {
        if (map.mode == SkeletonMapMode::Refused || recordedBones != map.sourceBones ||
            map.source.size() != map.targetBones || map.targetParents.size() != map.targetBones)
            return false;
        for (std::uint32_t bone = 0; bone < map.targetBones; ++bone)
        {
            const std::int32_t from = map.source[bone];
            if (from >= 0)
            {
                locals[bone] = recordedLocals[static_cast<std::uint32_t>(from)];
                objects[bone] = recordedObjects[static_cast<std::uint32_t>(from)];
                continue;
            }
            locals[bone] = restLocals[bone];
            const std::int32_t parent = map.targetParents[bone];
            objects[bone] = parent >= 0
                ? Compose(objects[static_cast<std::uint32_t>(parent)], locals[bone], CompositionOrder::ParentThenLocal, true)
                : locals[bone];
        }
        return true;
    }
}
