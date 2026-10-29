#pragma once

// The skeleton signature of a record against the puppet's skeleton.
//
// A matching bone count is not enough to arm the pose override "exact". The
// engine builds a ped skeleton from groups (the body, then one per attached
// piece: hat, hair, beard, satchel), and the order of the groups follows the
// order the pieces were put on. Two skeletons with the same count can hold
// the same groups in another order: player_zero has two signatures with 1050
// bones and 128 different parents in the block from bone 764. Armed by
// count, the record's locals of one group land on the bones of another: the
// hat above the head, the face stretched to the hat's bones.
//
// The signature is the parent table. The record keeps its layout's
// parents per bone (built from the engine table at capture, or discovered
// from the captured pairs); the puppet's comes from the finalizer detour
// (pose_capture.h RequestSkeletonInfo). Bones the record cannot vouch for
// are left out: object-space bones (kUnresolvedParent, a demoted bone),
// roots, and bones flagged ambiguous at discovery. Recordings show a
// noise of one bone between records of the same skeleton (bone 763 of the
// 874..979 humans, 648 of the 742s), so a skeleton differs only past
// kSignatureDifferLimit bones. A layout with more than kSignatureAmbiguousLimit
// ambiguous bones (riders: 550 to 590) or with fewer than half its bones
// comparable says nothing: unknown, armed by count.
//
// Pure: no engine access; unit tested in ReplayCoreTests and used offline by
// ReplayClipTool signature.

#include "skeleton_map.h"

#include <cstdint>
#include <vector>

namespace replay::pose
{
    inline constexpr std::uint32_t kSignatureDifferLimit = 8;
    inline constexpr std::uint32_t kSignatureAmbiguousLimit = 50;

    enum class SignatureVerdict : std::uint8_t
    {
        Same = 0,    // the parents agree (up to the noise limit)
        Differ = 1,  // same count, other skeleton: skeleton_signature_mismatch
        Unknown = 2, // the record's layout cannot vouch (ambiguous, too few bones, other count)
    };

    inline const char* SignatureVerdictText(SignatureVerdict verdict)
    {
        switch (verdict)
        {
        case SignatureVerdict::Same:
            return "same";
        case SignatureVerdict::Differ:
            return "differ";
        default:
            return "unknown";
        }
    }

    struct SignatureCompare
    {
        SignatureVerdict verdict = SignatureVerdict::Unknown;
        const char* reason = "none"; // why unknown: count, ambiguous, too_few
        std::uint32_t bones = 0;
        std::uint32_t compared = 0;  // bones looked at
        std::uint32_t skipped = 0;   // root, object space or ambiguous in the record
        std::uint32_t ambiguous = 0; // the record's ambiguous bones
        std::uint32_t differ = 0;    // compared bones with another parent
        std::int32_t firstDiff = -1;
        std::int32_t lastDiff = -1;
    };

    // Whether the record's layout can vouch for a skeleton at all, before
    // the puppet's table is asked for (the detour read costs a request
    // slot): null when it can, else the reason CompareSkeletonSignature
    // would give (ambiguous, too_few, count).
    inline const char* SignatureUnusable(const std::vector<std::int32_t>& recordParents, const std::vector<std::uint8_t>& ambiguous)
    {
        const std::uint32_t count = static_cast<std::uint32_t>(recordParents.size());
        if (count == 0)
            return "count";
        const bool hasAmbiguous = ambiguous.size() == count;
        std::uint32_t flagged = 0;
        std::uint32_t comparable = 0;
        for (std::uint32_t bone = 0; bone < count; ++bone)
        {
            const bool isAmbiguous = hasAmbiguous && ambiguous[bone] != 0;
            flagged += isAmbiguous ? 1u : 0u;
            const std::int32_t recorded = recordParents[bone];
            if (recorded >= 0 && static_cast<std::uint32_t>(recorded) < bone && !isAmbiguous)
                ++comparable;
        }
        if (flagged > kSignatureAmbiguousLimit)
            return "ambiguous";
        if (comparable * 2u < count)
            return "too_few";
        return nullptr;
    }

    // A record whose capture demoted a block of bones
    // mid-recording (PoseTrack::RebuiltBonesAt) beyond the noise limit:
    // the engine rebuilt that part of the original's skeleton under the
    // same bone count, so its later object-space bones are not the
    // puppet's bones of the same index. The map that keeps every other
    // bone 1:1 and leaves those to the puppet's own pose (rest), composed
    // from their parents as ApplySkeletonMap does. Refused when less than
    // half of the bones would be posed.
    inline std::uint32_t CountFlags(const std::vector<std::uint8_t>& flags)
    {
        std::uint32_t count = 0;
        for (const std::uint8_t flag : flags)
            count += flag ? 1u : 0u;
        return count;
    }

    template <typename PuppetParent>
    inline SkeletonMap BuildRebuiltMap(const PuppetParent* puppetParents, std::uint32_t count, const std::vector<std::uint8_t>& rebuilt)
    {
        SkeletonMap map;
        map.sourceBones = count;
        map.targetBones = count;
        if (!puppetParents || count == 0 || rebuilt.size() != count)
        {
            map.refuseReason = "empty";
            return map;
        }
        std::vector<std::int32_t> raw(count);
        for (std::uint32_t bone = 0; bone < count; ++bone)
            raw[bone] = static_cast<std::int32_t>(static_cast<std::int64_t>(puppetParents[bone]));
        NormaliseParents(raw.data(), count, map.targetParents);
        map.source.assign(count, -1);
        for (std::uint32_t bone = 0; bone < count; ++bone)
        {
            if (rebuilt[bone])
                continue;
            map.source[bone] = static_cast<std::int32_t>(bone);
            ++map.matched;
        }
        map.rest = count - map.matched;
        map.dropped = map.rest;
        map.sourceGroups = map.targetGroups = map.matchedGroups = 0;
        if (map.matched * 2u < count)
            map.refuseReason = "too_few_bones";
        else
        {
            map.mode = map.rest ? SkeletonMapMode::Mapped : SkeletonMapMode::Exact;
            map.refuseReason = "none";
        }
        return map;
    }

    // `recordParents`: the record layout's parent per bone (kRootParent -1,
    // kUnresolvedParent -2 or a bone index); `ambiguous`: one flag per bone
    // or empty. `puppetParents`: the engine table (a parent that is not an
    // earlier bone, 0xFFFF included, is no parent). Both tables as given,
    // counts compared here.
    template <typename PuppetParent>
    inline SignatureCompare CompareSkeletonSignature(const std::vector<std::int32_t>& recordParents,
        const std::vector<std::uint8_t>& ambiguous, const PuppetParent* puppetParents, std::uint32_t puppetCount)
    {
        SignatureCompare out;
        const std::uint32_t count = static_cast<std::uint32_t>(recordParents.size());
        out.bones = count;
        if (count == 0 || !puppetParents || puppetCount != count)
        {
            out.reason = "count";
            return out;
        }
        const bool hasAmbiguous = ambiguous.size() == count;
        for (std::uint32_t bone = 0; bone < count; ++bone)
        {
            const bool isAmbiguous = hasAmbiguous && ambiguous[bone] != 0;
            out.ambiguous += isAmbiguous ? 1u : 0u;
            const std::int32_t recorded = recordParents[bone];
            if (recorded < 0 || static_cast<std::uint32_t>(recorded) >= bone || isAmbiguous)
            {
                ++out.skipped;
                continue;
            }
            ++out.compared;
            const std::int64_t raw = static_cast<std::int64_t>(puppetParents[bone]);
            const std::int32_t puppet = raw >= 0 && raw < static_cast<std::int64_t>(bone) ? static_cast<std::int32_t>(raw) : -1;
            if (puppet == recorded)
                continue;
            ++out.differ;
            if (out.firstDiff < 0)
                out.firstDiff = static_cast<std::int32_t>(bone);
            out.lastDiff = static_cast<std::int32_t>(bone);
        }
        if (out.ambiguous > kSignatureAmbiguousLimit)
            out.reason = "ambiguous";
        else if (out.compared * 2u < count)
            out.reason = "too_few";
        else
            out.verdict = out.differ > kSignatureDifferLimit ? SignatureVerdict::Differ : SignatureVerdict::Same;
        return out;
    }
}
