#pragma once

// PoseTrack v2 skeleton layout.
//
// The recorder stores bone-local transforms (they change sparsely: a walking
// ped leaves fingers, face and most props untouched) and rebuilds object
// transforms on playback. That needs each bone's parent and the composition
// rule. Neither is read from engine structures: both are discovered from
// captured (local, object) pairs and cross-checked over several frames.
// Bones whose object transform cannot be explained by any parent are kept as
// "unresolved" and recorded directly in object space, so a wrong guess can
// never silently corrupt the pose. During capture every frame is checked
// again and a bone that stops matching is demoted to unresolved.

#include "pose_math.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace replay::pose
{
    constexpr std::int32_t kRootParent = -1;
    constexpr std::int32_t kUnresolvedParent = -2;

    struct SkeletonLayout
    {
        std::uint32_t boneCount = 0;
        CompositionOrder order = CompositionOrder::ParentThenLocal;
        bool inheritScale = false;
        // kRootParent: object == local. kUnresolvedParent: recorded in
        // object space. Otherwise the parent bone index.
        std::vector<std::int32_t> parent;
        // More than one parent explained the discovery frames; the pick used
        // the index tie-break and is the first suspect if it later fails.
        std::vector<std::uint8_t> ambiguous;
        // Bones with a large subtree use 64-bit rotations: their error is
        // amplified along every descendant.
        std::vector<std::uint8_t> highPrecision;
        // Bone indices ordered so every parent precedes its children.
        std::vector<std::uint32_t> evaluationOrder;
        std::vector<std::uint32_t> unresolvedBones;

        std::uint32_t ChannelCount() const
        {
            return boneCount + static_cast<std::uint32_t>(unresolvedBones.size());
        }
    };

    struct DiscoveryConfig
    {
        float translationTolerance = 0.001f;
        float rotationTolerance = 0.002f;
        float scaleTolerance = 0.001f;
        std::uint32_t highPrecisionSubtree = 8;
    };

    struct DiscoveryFrame
    {
        const BoneQts* locals = nullptr;
        const BoneQts* objects = nullptr;
    };

    struct DiscoveryStats
    {
        std::uint32_t roots = 0;
        std::uint32_t parented = 0;
        std::uint32_t unresolved = 0;
        std::uint32_t ambiguous = 0;
        std::uint32_t highPrecision = 0;
        CompositionOrder order = CompositionOrder::ParentThenLocal;
        bool inheritScale = false;
    };

    // Needs at least two frames with different poses; more frames reduce
    // ambiguity. Returns false only for invalid input.
    bool DiscoverSkeletonLayout(
        const DiscoveryFrame* frames,
        std::uint32_t frameCount,
        std::uint32_t boneCount,
        const DiscoveryConfig& config,
        SkeletonLayout& layout,
        DiscoveryStats& stats);

    // Whether an existing layout (another ped of the same skeleton) explains every
    // frame with the discovery tolerances, root and parented bones alike
    // (object-space bones always do). Lets a new record skip the O(bones^2) search;
    // runtime demotion still checks every captured frame afterwards.
    bool LayoutExplainsFrames(
        const SkeletonLayout& layout,
        const DiscoveryFrame* frames,
        std::uint32_t frameCount,
        std::uint32_t boneCount,
        const DiscoveryConfig& config);

    // Counts of a layout, for the stats of a reused one.
    DiscoveryStats DescribeLayout(const SkeletonLayout& layout);

    // The layout the engine's own parent table gives (the pose finalizer's
    // indices): ParentThenLocal with inherited scale, bone 0 the root (or in object
    // space when the skeleton's root scale is not 1, since its object is the scaled
    // local), a parent that is not an earlier bone in object space. High precision
    // by subtree size as in discovery.
    // Subtree sizes, high precision flags and counts of a layout whose parents and
    // evaluation order are set (used by discovery and by BuildLayoutFromParents).
    void FinishLayout(SkeletonLayout& layout, const DiscoveryConfig& config, DiscoveryStats& stats);

    bool BuildLayoutFromParents(
        const std::uint16_t* parents,
        std::uint32_t boneCount,
        bool rootInObjectSpace,
        const DiscoveryConfig& config,
        SkeletonLayout& layout,
        DiscoveryStats& stats);

    // Same bone count, composition and parent per bone.
    bool SameLayout(const SkeletonLayout& a, const SkeletonLayout& b);

    // Among the candidate layouts that explain the frames, the one with the fewest
    // object-space bones (then the most parented); `count` when none explains them.
    // Trying every candidate matters: a layout demoted for one ped explains any
    // frames on its demoted bones, and reusing it would cost every later ped those
    // bones in object space.
    std::size_t PickReusableLayout(
        const SkeletonLayout* const* candidates,
        std::size_t count,
        const DiscoveryFrame* frames,
        std::uint32_t frameCount,
        std::uint32_t boneCount,
        const DiscoveryConfig& config);

    // Rebuilds object transforms from locals. `unresolvedObjects` holds one
    // object transform per layout.unresolvedBones entry.
    void ComposeObjects(
        const SkeletonLayout& layout,
        const BoneQts* locals,
        const BoneQts* unresolvedObjects,
        BoneQts* objects);

    struct CompositionError
    {
        float maxTranslation = 0.0f;
        float maxRotation = 0.0f;
    };

    // Compares engine object transforms against the layout's reconstruction.
    CompositionError MeasureCompositionError(
        const SkeletonLayout& layout,
        const BoneQts* locals,
        const BoneQts* objects);

    // Parented bones whose engine object no longer equals parent-object
    // composed with the local (checked per bone against the engine parent,
    // so one bad bone does not blame its descendants).
    std::vector<std::uint32_t> FindFailingBones(
        const SkeletonLayout& layout,
        const BoneQts* locals,
        const BoneQts* objects,
        float translationTolerance,
        float rotationTolerance);

    // What differed from the previous frame in FindFailingBonesChanged, for the
    // evidence of the script reads: bones whose local differed, bones whose object
    // differed (byte compare of the 48-byte records), and whether bone 0 (the root)
    // differed in either.
    struct ChangedBoneCounts
    {
        std::uint32_t locals = 0;
        std::uint32_t objects = 0;
        bool root = false;
    };

    // FindFailingBones on a frame whose previous validated frame (same layout) had
    // no failing bone. A bone's result depends only on its local, its object and
    // its parent's object (ExpectedObject), so a bone whose three records are
    // byte-equal to that frame's passes again and is not recomputed. Same result as
    // FindFailingBones bit for bit. `checked` counts the bones recomputed. The
    // previous frame's records of the bones that differ are overwritten with this
    // frame's, so the arrays hold this frame afterwards (the caller keeps them only
    // when nothing failed).
    // The tolerance tests are TranslationExceeds and AngleExceeds (the same answer
    // as TranslationDistance > t and AngleBetween > r, with the exact functions near
    // the threshold). Both blocks byte-equal to the previous ones (an idle copy):
    // nothing is checked. The per-bone byte compares use SSE. A parented bone's
    // expected object is composed with SSE (ComposeSimd) and ValidationVerdict
    // decides when it is clearly away from both tolerances; near them, for a root
    // bone and for degenerate input the scalar ExpectedObject decides.
    std::vector<std::uint32_t> FindFailingBonesChanged(
        const SkeletonLayout& layout,
        const BoneQts* locals,
        const BoneQts* objects,
        BoneQts* previousLocals,
        BoneQts* previousObjects,
        float translationTolerance,
        float rotationTolerance,
        std::uint32_t& checked,
        ChangedBoneCounts* counts = nullptr);

    // ComposeSimd is the scalar Compose bit for bit in the tests (/fp:precise), but
    // the ASI builds with /fp:fast, where the compiler may reorder the float
    // operations of either: the two expected objects can then differ by a few ulps,
    // and a bone right at a tolerance could be decided either way. The band moves
    // both thresholds away by more than that difference:
    // - translation: with u = 2^-24, either computation of the expected
    //   translation is within u (|parent t| + |offset|) + 14 u |offset| of
    //   the exact composition per axis for any order of its four-term sums
    //   (offset: the local translation rotated by the parent), so the two
    //   are less than 51 u (3.0e-6) of the extent apart, the extent being
    //   |parent t| + |offset| in L1 norms (the bound of a fully expanded
    //   rotation is 7.2e-6). The margin is kBandTranslationRelative times
    //   the extent plus kBandTranslationAbsolute, which also covers the
    //   rounding of the baseline's and the band's squared distances (a few
    //   ulps of 2 mm);
    // - rotation: either normalized product is within 18 u (rad) of the
    //   exact rotation (16 u from the four-term sums of the product, 2 u
    //   from the normalization's roundings), so the two are less than 36 u
    //   (2.1e-6 rad) apart, and AngleBetween's own error is about 18 u
    //   (1.1e-6 rad) more: the margin kBandAngle is 6 times their sum. Both
    //   moved thresholds are tested with the cheap angle test (AngleExceeds'
    //   band covers its own rounding).
    // The band leaves a test undecided when its inputs hold NaN or infinity,
    // a quaternion's squared norm is outside (0.5, 2) or the extent is
    // kBandExtentCap or more; the bone still fails when the other test
    // clearly exceeds its threshold (the baseline's result either way).
    constexpr float kBandTranslationAbsolute = 1e-5f; // m
    constexpr float kBandTranslationRelative = 2e-5f; // of the extent
    constexpr float kBandAngle = 2e-5f; // rad
    constexpr float kBandExtentCap = 1e6f; // m

    struct ValidationBand
    {
        float translationTolerance = 0.0f;
        float rotationTolerance = 0.0f;
        AngleTest above; // rotationTolerance + kBandAngle
        AngleTest below; // rotationTolerance - kBandAngle
        // false (a tolerance the band cannot move): every bone takes the
        // scalar test.
        bool usable = false;
    };
    ValidationBand MakeValidationBand(float translationTolerance, float rotationTolerance);

    // FindFailingBonesChanged's test of a parented bone (TranslationExceeds
    // or AngleExceeds of its scalar expected object against `object`),
    // decided from `composed`, the ComposeSimd of `parentObject` and
    // `local`: 1 fails, -1 passes, 0 undecided (near a moved threshold, a
    // quaternion far from unit, NaN, infinity, a huge translation): the
    // caller runs the scalar test.
    int ValidationVerdict(
        const ValidationBand& band,
        const BoneQts& parentObject,
        const BoneQts& local,
        const BoneQts& composed,
        const BoneQts& object);

    // Tests only: called on every SSE expected object FindFailingBonesChanged
    // composes (with the parent object it was composed from), before
    // ValidationVerdict; moving it by a few ulps imitates /fp:fast. Null,
    // the default, in the mod.
    using ExpectedObjectHook = void (*)(BoneQts& composed, const BoneQts& parentObject, std::uint32_t bone);
    void SetExpectedObjectHook(ExpectedObjectHook hook);

    // What the engine did to a skeleton after its pose finalizer. `finalizerLocals`
    // are the locals the finalizer had just written (the detour's copy),
    // `lateLocals`/`lateObjects` the same buffer at the end of the frame (read from
    // the script thread before the finalizer ran again). A bone counts as a local
    // change when its local moved, and as an object change when its end-of-frame
    // object is not its parent's end-of-frame object composed with its end-of-frame
    // local (the finalizer's own rule with the engine parent table: an object
    // written directly, as ragdoll or IK code does). The worst bone is the one with
    // the largest translation of either kind.
    struct LatePoseChange
    {
        std::uint32_t localBones = 0;
        std::uint32_t objectBones = 0;
        std::int32_t worstBone = -1;
        float maxTranslation = 0.0f;
        float maxRotation = 0.0f;
    };
    LatePoseChange CompareLatePose(
        const BoneQts* finalizerLocals,
        const BoneQts* lateLocals,
        const BoneQts* lateObjects,
        const std::uint16_t* engineParents,
        std::uint32_t boneCount,
        float rootScale,
        float translationTolerance,
        float rotationTolerance,
        std::vector<BoneQts>& scratch);

    // The playback side of the same question. `written` is the pose the override
    // wrote (boneCount locals, then boneCount objects), `now` the puppet's buffer at
    // the end of the frame. Every bone is compared as written, local and object (a
    // recorded object is not always its parent composed with its local: bones
    // demoted to object space), so any difference is an engine write after ours.
    // firstBone and lastBone bound the changed bones (a layer's bones are one
    // contiguous group, e.g. the skinned pelt of a carcass 259..502).
    struct WrittenPoseChange
    {
        std::uint32_t localBones = 0;
        std::uint32_t objectBones = 0;
        std::int32_t firstBone = -1;
        std::int32_t lastBone = -1;
        std::int32_t worstBone = -1;
        float maxTranslation = 0.0f;
        float maxRotation = 0.0f;
    };
    WrittenPoseChange CompareWrittenPose(
        const BoneQts* written,
        const BoneQts* now,
        std::uint32_t boneCount,
        float translationTolerance,
        float rotationTolerance);

    // Moves bones to object-space recording and rebuilds the evaluation order.
    void DemoteBones(SkeletonLayout& layout, const std::vector<std::uint32_t>& bones);

    // For object-space bones that were demoted, a parent that explains every frame
    // given (the discovery samples plus the frames that contradicted the old
    // parent), with the layout's own composition rule; a bone with no such parent
    // stays in object space. A parent inside the bone's own subtree is never taken.
    // Returns the bones re-parented; the evaluation order and unresolved list are rebuilt.
    std::uint32_t ReparentBones(
        SkeletonLayout& layout,
        const std::vector<std::uint32_t>& bones,
        const DiscoveryFrame* frames,
        std::uint32_t frameCount,
        const DiscoveryConfig& config);
}
