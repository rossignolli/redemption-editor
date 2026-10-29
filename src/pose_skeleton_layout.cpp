#include "pose_skeleton_layout.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <emmintrin.h>
#include <limits>

namespace replay::pose
{
    namespace
    {
        struct Rule
        {
            CompositionOrder order = CompositionOrder::ParentThenLocal;
            bool inheritScale = false;
        };

        bool Matches(
            const BoneQts& expected,
            const BoneQts& actual,
            const DiscoveryConfig& config,
            bool compareScale,
            float& error)
        {
            const float translation = TranslationDistance(expected, actual);
            const float rotation = AngleBetween(expected.rotation, actual.rotation);
            error = translation + rotation;
            if (translation > config.translationTolerance ||
                rotation > config.rotationTolerance)
            {
                return false;
            }
            if (compareScale)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (std::fabs(expected.scale[axis] - actual.scale[axis]) >
                        config.scaleTolerance)
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        struct Candidate
        {
            std::int32_t parent = kUnresolvedParent;
            float error = 0.0f;
        };

        // Candidate parents of `bone` that explain every frame.
        std::vector<Candidate> FindCandidates(
            const DiscoveryFrame* frames,
            std::uint32_t frameCount,
            std::uint32_t boneCount,
            std::uint32_t bone,
            Rule rule,
            const DiscoveryConfig& config)
        {
            std::vector<Candidate> candidates;
            float error = 0.0f;

            bool root = true;
            float rootError = 0.0f;
            for (std::uint32_t frame = 0; frame < frameCount && root; ++frame)
            {
                root = Matches(
                    frames[frame].locals[bone],
                    frames[frame].objects[bone],
                    config,
                    rule.inheritScale,
                    error);
                rootError += error;
            }
            if (root)
                candidates.push_back({kRootParent, rootError});

            for (std::uint32_t parent = 0; parent < boneCount; ++parent)
            {
                if (parent == bone)
                    continue;
                bool all = true;
                float total = 0.0f;
                for (std::uint32_t frame = 0; frame < frameCount && all; ++frame)
                {
                    const BoneQts composed = Compose(
                        frames[frame].objects[parent],
                        frames[frame].locals[bone],
                        rule.order,
                        rule.inheritScale);
                    all = Matches(
                        composed, frames[frame].objects[bone], config, rule.inheritScale, error);
                    total += error;
                }
                if (all)
                    candidates.push_back({static_cast<std::int32_t>(parent), total});
            }
            return candidates;
        }

        // Among parents that fit, the one with the smallest error; near ties
        // (co-located helper bones) go to the closest preceding index, which
        // is where depth-first skeletons put the real parent.
        Candidate PickCandidate(const std::vector<Candidate>& candidates, std::uint32_t bone)
        {
            float best = std::numeric_limits<float>::max();
            bool hasParent = false;
            for (const Candidate& candidate : candidates)
            {
                if (candidate.parent >= 0)
                {
                    hasParent = true;
                    best = std::min(best, candidate.error);
                }
            }
            if (!hasParent)
                return candidates.front(); // only the root candidate

            const float window = best * 2.0f + 1e-5f;
            auto rank = [bone](std::int32_t parent) {
                const auto index = static_cast<std::uint32_t>(parent);
                return index < bone
                    ? static_cast<std::uint64_t>(bone - index)
                    : (1ull << 32) + index;
            };
            Candidate pick{};
            std::uint64_t pickRank = ~0ull;
            for (const Candidate& candidate : candidates)
            {
                if (candidate.parent < 0 || candidate.error > window)
                    continue;
                const std::uint64_t candidateRank = rank(candidate.parent);
                if (candidateRank < pickRank)
                {
                    pick = candidate;
                    pickRank = candidateRank;
                }
            }
            return pick;
        }

        bool TryBuildEvaluationOrder(SkeletonLayout& layout)
        {
            const std::uint32_t count = layout.boneCount;
            std::vector<std::uint8_t> state(count, 0); // 0 new, 1 on chain, 2 done
            layout.evaluationOrder.clear();
            layout.evaluationOrder.reserve(count);

            std::vector<std::uint32_t> chain;
            for (std::uint32_t start = 0; start < count; ++start)
            {
                chain.clear();
                std::uint32_t bone = start;
                while (state[bone] != 2)
                {
                    if (state[bone] == 1)
                    {
                        // Cycle: record this bone in object space and retry.
                        layout.parent[bone] = kUnresolvedParent;
                        return false;
                    }
                    state[bone] = 1;
                    chain.push_back(bone);
                    const std::int32_t parent = layout.parent[bone];
                    if (parent < 0)
                        break;
                    bone = static_cast<std::uint32_t>(parent);
                }
                for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                {
                    state[*it] = 2;
                    layout.evaluationOrder.push_back(*it);
                }
            }
            return true;
        }

        void RebuildDerived(SkeletonLayout& layout)
        {
            while (!TryBuildEvaluationOrder(layout))
            {
            }
            layout.unresolvedBones.clear();
            for (std::uint32_t bone = 0; bone < layout.boneCount; ++bone)
            {
                if (layout.parent[bone] == kUnresolvedParent)
                    layout.unresolvedBones.push_back(bone);
            }
        }

        BoneQts ExpectedObject(
            const SkeletonLayout& layout,
            std::uint32_t bone,
            const BoneQts* locals,
            const BoneQts* objects)
        {
            const std::int32_t parent = layout.parent[bone];
            if (parent == kRootParent)
                return locals[bone];
            return Compose(
                objects[static_cast<std::uint32_t>(parent)],
                locals[bone],
                layout.order,
                layout.inheritScale);
        }
    }

    bool DiscoverSkeletonLayout(
        const DiscoveryFrame* frames,
        std::uint32_t frameCount,
        std::uint32_t boneCount,
        const DiscoveryConfig& config,
        SkeletonLayout& layout,
        DiscoveryStats& stats)
    {
        layout = {};
        stats = {};
        if (!frames || frameCount < 2 || boneCount == 0)
            return false;
        for (std::uint32_t frame = 0; frame < frameCount; ++frame)
        {
            if (!frames[frame].locals || !frames[frame].objects)
                return false;
        }

        // Pick the composition rule that explains more bones.
        const Rule rules[4] = {
            {CompositionOrder::ParentThenLocal, false},
            {CompositionOrder::LocalThenParent, false},
            {CompositionOrder::ParentThenLocal, true},
            {CompositionOrder::LocalThenParent, true},
        };
        // The same rule the four full passes would pick (the one explaining
        // the most bones), at one to two passes instead of four (four passes
        // cost ~250 ms per metaped on one thread). A first rule is chosen on a sample of
        // bones and searched in full; another rule can only beat it if it
        // explains a bone the first one leaves unexplained (otherwise its
        // bones are a subset), so the other rules are tried on those bones
        // alone and searched in full only when one of them fits.
        std::vector<std::vector<Candidate>> perRule[4];
        std::vector<std::uint8_t> done[4];
        std::uint32_t explained[4] = {};
        bool full[4] = {};
        const std::uint32_t stride = std::max<std::uint32_t>(1u, boneCount / 64u);
        std::uint32_t sampled[4] = {};
        auto search = [&](int ruleIndex, std::uint32_t bone) -> const std::vector<Candidate>& {
            if (!done[ruleIndex][bone])
            {
                perRule[ruleIndex][bone] =
                    FindCandidates(frames, frameCount, boneCount, bone, rules[ruleIndex], config);
                done[ruleIndex][bone] = 1;
            }
            return perRule[ruleIndex][bone];
        };
        for (int ruleIndex = 0; ruleIndex < 4; ++ruleIndex)
        {
            perRule[ruleIndex].resize(boneCount);
            done[ruleIndex].assign(boneCount, 0);
            for (std::uint32_t bone = 0; bone < boneCount; bone += stride)
            {
                if (!search(ruleIndex, bone).empty())
                    ++sampled[ruleIndex];
            }
        }
        auto runFull = [&](int ruleIndex) {
            if (full[ruleIndex])
                return;
            full[ruleIndex] = true;
            explained[ruleIndex] = 0;
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                if (!search(ruleIndex, bone).empty())
                    ++explained[ruleIndex];
            }
        };
        int first = 0;
        for (int ruleIndex = 1; ruleIndex < 4; ++ruleIndex)
        {
            if (sampled[ruleIndex] > sampled[first])
                first = ruleIndex;
        }
        runFull(first);
        for (int ruleIndex = 0; ruleIndex < 4; ++ruleIndex)
        {
            if (ruleIndex == first)
                continue;
            bool promising = false;
            for (std::uint32_t bone = 0; bone < boneCount && !promising; ++bone)
            {
                if (perRule[first][bone].empty() && !search(ruleIndex, bone).empty())
                    promising = true;
            }
            if (promising)
                runFull(ruleIndex);
        }
        int chosen = 0;
        for (int ruleIndex = 0; ruleIndex < 4; ++ruleIndex)
        {
            if (!full[ruleIndex])
                continue;
            if (!full[chosen] || explained[ruleIndex] > explained[chosen])
                chosen = ruleIndex;
        }

        layout.boneCount = boneCount;
        layout.order = rules[chosen].order;
        layout.inheritScale = rules[chosen].inheritScale;
        layout.parent.assign(boneCount, kUnresolvedParent);
        layout.ambiguous.assign(boneCount, 0);
        layout.highPrecision.assign(boneCount, 0);
        stats.order = layout.order;
        stats.inheritScale = layout.inheritScale;

        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            const std::vector<Candidate>& candidates = perRule[chosen][bone];
            if (candidates.empty())
                continue;
            if (candidates.size() > 1)
            {
                layout.ambiguous[bone] = 1;
                ++stats.ambiguous;
            }
            layout.parent[bone] = PickCandidate(candidates, bone).parent;
        }

        RebuildDerived(layout);
        FinishLayout(layout, config, stats);
        return true;
    }

    namespace
    {
        // Subtree sizes (children after parents in evaluationOrder, so it is
        // walked backwards), the high precision flags and the counts.
        void FinishLayoutImpl(SkeletonLayout& layout, const DiscoveryConfig& config, DiscoveryStats& stats)
        {
            const std::uint32_t boneCount = layout.boneCount;
            std::vector<std::uint32_t> subtree(boneCount, 1);
            for (auto it = layout.evaluationOrder.rbegin();
                it != layout.evaluationOrder.rend();
                ++it)
            {
                const std::int32_t parent = layout.parent[*it];
                if (parent >= 0)
                    subtree[static_cast<std::uint32_t>(parent)] += subtree[*it];
            }
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                const std::int32_t parent = layout.parent[bone];
                if (parent == kRootParent)
                    ++stats.roots;
                else if (parent == kUnresolvedParent)
                    ++stats.unresolved;
                else
                    ++stats.parented;

                if (subtree[bone] >= config.highPrecisionSubtree)
                {
                    layout.highPrecision[bone] = 1;
                    ++stats.highPrecision;
                }
            }
        }
    }

    void FinishLayout(SkeletonLayout& layout, const DiscoveryConfig& config, DiscoveryStats& stats)
    {
        FinishLayoutImpl(layout, config, stats);
    }

    bool BuildLayoutFromParents(
        const std::uint16_t* parents,
        std::uint32_t boneCount,
        bool rootInObjectSpace,
        const DiscoveryConfig& config,
        SkeletonLayout& layout,
        DiscoveryStats& stats)
    {
        layout = {};
        stats = {};
        if (!parents || boneCount == 0)
            return false;
        layout.boneCount = boneCount;
        layout.order = CompositionOrder::ParentThenLocal;
        layout.inheritScale = true;
        layout.parent.assign(boneCount, kUnresolvedParent);
        layout.ambiguous.assign(boneCount, 0);
        layout.highPrecision.assign(boneCount, 0);
        stats.order = layout.order;
        stats.inheritScale = true;
        layout.parent[0] = rootInObjectSpace ? kUnresolvedParent : kRootParent;
        for (std::uint32_t bone = 1; bone < boneCount; ++bone)
        {
            if (parents[bone] < bone)
                layout.parent[bone] = static_cast<std::int32_t>(parents[bone]);
        }
        RebuildDerived(layout);
        FinishLayoutImpl(layout, config, stats);
        return true;
    }

    void ComposeObjects(
        const SkeletonLayout& layout,
        const BoneQts* locals,
        const BoneQts* unresolvedObjects,
        BoneQts* objects)
    {
        for (std::size_t index = 0; index < layout.unresolvedBones.size(); ++index)
            objects[layout.unresolvedBones[index]] = unresolvedObjects[index];

        for (const std::uint32_t bone : layout.evaluationOrder)
        {
            const std::int32_t parent = layout.parent[bone];
            if (parent == kRootParent)
                objects[bone] = locals[bone];
            else if (parent >= 0)
                objects[bone] = Compose(
                    objects[static_cast<std::uint32_t>(parent)],
                    locals[bone],
                    layout.order,
                    layout.inheritScale);
        }
    }

    CompositionError MeasureCompositionError(
        const SkeletonLayout& layout,
        const BoneQts* locals,
        const BoneQts* objects)
    {
        std::vector<BoneQts> unresolved;
        unresolved.reserve(layout.unresolvedBones.size());
        for (const std::uint32_t bone : layout.unresolvedBones)
            unresolved.push_back(objects[bone]);

        std::vector<BoneQts> rebuilt(layout.boneCount);
        ComposeObjects(layout, locals, unresolved.data(), rebuilt.data());

        CompositionError error;
        for (std::uint32_t bone = 0; bone < layout.boneCount; ++bone)
        {
            error.maxTranslation = std::max(
                error.maxTranslation,
                TranslationDistance(rebuilt[bone], objects[bone]));
            error.maxRotation = std::max(
                error.maxRotation,
                AngleBetween(rebuilt[bone].rotation, objects[bone].rotation));
        }
        return error;
    }

    std::vector<std::uint32_t> FindFailingBones(
        const SkeletonLayout& layout,
        const BoneQts* locals,
        const BoneQts* objects,
        float translationTolerance,
        float rotationTolerance)
    {
        std::vector<std::uint32_t> failing;
        for (std::uint32_t bone = 0; bone < layout.boneCount; ++bone)
        {
            if (layout.parent[bone] == kUnresolvedParent)
                continue;
            const BoneQts expected = ExpectedObject(layout, bone, locals, objects);
            if (TranslationDistance(expected, objects[bone]) > translationTolerance ||
                AngleBetween(expected.rotation, objects[bone].rotation) > rotationTolerance)
            {
                failing.push_back(bone);
            }
        }
        return failing;
    }

    namespace
    {
        // The 48 bytes of two bone records are equal (pose_stream.cpp
        // SameBone).
        inline bool SameBone(const BoneQts& a, const BoneQts& b)
        {
            const auto* pa = reinterpret_cast<const __m128i*>(&a);
            const auto* pb = reinterpret_cast<const __m128i*>(&b);
            const __m128i e0 = _mm_cmpeq_epi8(_mm_loadu_si128(pa), _mm_loadu_si128(pb));
            const __m128i e1 = _mm_cmpeq_epi8(_mm_loadu_si128(pa + 1), _mm_loadu_si128(pb + 1));
            const __m128i e2 = _mm_cmpeq_epi8(_mm_loadu_si128(pa + 2), _mm_loadu_si128(pb + 2));
            return _mm_movemask_epi8(_mm_and_si128(_mm_and_si128(e0, e1), e2)) == 0xFFFF;
        }

        // A float's bits. Non-negative floats order like
        // their bits, with infinity and NaN above every finite one, so the
        // band's range checks hold under /fp:fast too (where the compiler
        // may drop the NaN case of a float comparison).
        inline std::uint32_t FloatBits(float value)
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return bits;
        }

        constexpr std::uint32_t kInfinityBits = 0x7F800000u;

        // conj(a) * b, Multiply(Conjugate(a), b) term by term with SSE:
        //   x = ((aw*bx - ax*bw) - ay*bz) + az*by
        //   y = ((aw*by + ax*bz) - ay*bw) - az*bx
        //   z = ((aw*bz - ax*by) + ay*bx) - az*bw
        //   w = ((aw*bw + ax*bx) + ay*by) + az*bz
        inline __m128 RelativeRotation(__m128 a, __m128 b)
        {
            const __m128 aw = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 3, 3, 3));
            const __m128 ax = _mm_shuffle_ps(a, a, _MM_SHUFFLE(0, 0, 0, 0));
            const __m128 ay = _mm_shuffle_ps(a, a, _MM_SHUFFLE(1, 1, 1, 1));
            const __m128 az = _mm_shuffle_ps(a, a, _MM_SHUFFLE(2, 2, 2, 2));
            const __m128 t1 = _mm_mul_ps(aw, b);
            const __m128 t2 = _mm_mul_ps(ax, _mm_shuffle_ps(b, b, _MM_SHUFFLE(0, 1, 2, 3))); // bw bz by bx
            const __m128 t3 = _mm_mul_ps(ay, _mm_shuffle_ps(b, b, _MM_SHUFFLE(1, 0, 3, 2))); // bz bw bx by
            const __m128 t4 = _mm_mul_ps(az, _mm_shuffle_ps(b, b, _MM_SHUFFLE(2, 3, 0, 1))); // by bx bw bz
            const int minus = static_cast<int>(0x80000000u);
            const __m128 s2 = _mm_castsi128_ps(_mm_set_epi32(0, minus, 0, minus)); // - + - +
            const __m128 s3 = _mm_castsi128_ps(_mm_set_epi32(0, 0, minus, minus)); // - - + +
            const __m128 s4 = _mm_castsi128_ps(_mm_set_epi32(0, minus, minus, 0)); // + - - +
            return _mm_add_ps(
                _mm_add_ps(_mm_add_ps(t1, _mm_xor_ps(t2, s2)), _mm_xor_ps(t3, s3)), _mm_xor_ps(t4, s4));
        }

        std::atomic<ExpectedObjectHook> g_expectedObjectHook{nullptr};
    }

    ValidationBand MakeValidationBand(float translationTolerance, float rotationTolerance)
    {
        ValidationBand band;
        band.translationTolerance = translationTolerance;
        band.rotationTolerance = rotationTolerance;
        band.above = MakeAngleTest(rotationTolerance + kBandAngle);
        band.below = MakeAngleTest(rotationTolerance - kBandAngle);
        band.usable = band.above.cheap && band.below.cheap && translationTolerance > 0.0f &&
            translationTolerance < 1000.0f;
        return band;
    }

    int ValidationVerdict(
        const ValidationBand& band,
        const BoneQts& parentObject,
        const BoneQts& local,
        const BoneQts& composed,
        const BoneQts& object)
    {
        if (!band.usable)
            return 0;
        // Translation: the squared distance to the object, and the extent
        // the margin grows with, |parent t| + |composed t - parent t| (L1
        // norms over x, y, z; the second is the composed offset).
        const __m128 xyz = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
        const __m128 magnitude = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
        const __m128 composedT = _mm_loadu_ps(composed.translation);
        const __m128 parentT = _mm_loadu_ps(parentObject.translation);
        const __m128 delta = _mm_and_ps(_mm_sub_ps(composedT, _mm_loadu_ps(object.translation)), xyz);
        const __m128 extent = _mm_and_ps(
            _mm_add_ps(_mm_and_ps(parentT, magnitude), _mm_and_ps(_mm_sub_ps(composedT, parentT), magnitude)), xyz);
        const __m128 squares = _mm_mul_ps(delta, delta);
        // Lane 0 the squared distance, lane 1 the extent.
        __m128 sums = _mm_add_ps(_mm_unpacklo_ps(squares, extent), _mm_unpackhi_ps(squares, extent));
        sums = _mm_add_ps(sums, _mm_movehl_ps(sums, sums));
        const float squared = _mm_cvtss_f32(sums);
        const float extentSum = _mm_cvtss_f32(_mm_shuffle_ps(sums, sums, _MM_SHUFFLE(1, 1, 1, 1)));
        int translation = 0;
        if (FloatBits(squared) < kInfinityBits && FloatBits(extentSum) < FloatBits(kBandExtentCap))
        {
            const float margin = kBandTranslationAbsolute + kBandTranslationRelative * extentSum;
            const float above = band.translationTolerance + margin;
            // The factors cover the rounding of both sides (a few ulps).
            if (squared > above * above * 1.00001f)
            {
                translation = 1;
            }
            else if (margin < band.translationTolerance)
            {
                const float below = band.translationTolerance - margin;
                if (squared < below * below * 0.99999f)
                    translation = -1;
            }
        }
        if (translation > 0)
            return 1;
        // Rotation: the cheap angle test (AngleExceeds) against both moved
        // thresholds, only with every quaternion near unit (the squared
        // norms of the composed, object, parent and local rotations inside
        // (0.5, 2), which also rules out NaN and infinity).
        const __m128 composedQ = _mm_loadu_ps(&composed.rotation.x);
        const __m128 objectQ = _mm_loadu_ps(&object.rotation.x);
        const __m128 parentQ = _mm_loadu_ps(&parentObject.rotation.x);
        const __m128 localQ = _mm_loadu_ps(&local.rotation.x);
        __m128 n0 = _mm_mul_ps(composedQ, composedQ);
        __m128 n1 = _mm_mul_ps(objectQ, objectQ);
        __m128 n2 = _mm_mul_ps(parentQ, parentQ);
        __m128 n3 = _mm_mul_ps(localQ, localQ);
        _MM_TRANSPOSE4_PS(n0, n1, n2, n3);
        const __m128 norms = _mm_add_ps(_mm_add_ps(n0, n1), _mm_add_ps(n2, n3));
        const __m128 inRange = _mm_and_ps(
            _mm_cmpgt_ps(norms, _mm_set1_ps(0.5f)), _mm_cmplt_ps(norms, _mm_set1_ps(2.0f)));
        if (_mm_movemask_ps(inRange) != 0xF)
            return 0;
        const __m128 relative = RelativeRotation(composedQ, objectQ);
        const __m128 relativeSquares = _mm_mul_ps(relative, relative);
        const __m128 vectorSquares = _mm_and_ps(relativeSquares, xyz);
        __m128 vectorSum = _mm_add_ps(vectorSquares, _mm_movehl_ps(vectorSquares, vectorSquares));
        vectorSum = _mm_add_ss(vectorSum, _mm_shuffle_ps(vectorSum, vectorSum, _MM_SHUFFLE(1, 1, 1, 1)));
        const float vector = _mm_cvtss_f32(vectorSum);
        const float w2 = _mm_cvtss_f32(_mm_shuffle_ps(relativeSquares, relativeSquares, _MM_SHUFFLE(3, 3, 3, 3)));
        if (vector > w2 * band.above.tanHalfSquared + band.above.band)
            return 1;
        if (translation < 0 && vector < w2 * band.below.tanHalfSquared - band.below.band)
            return -1;
        return 0;
    }

    void SetExpectedObjectHook(ExpectedObjectHook hook)
    {
        g_expectedObjectHook.store(hook, std::memory_order_relaxed);
    }

    std::vector<std::uint32_t> FindFailingBonesChanged(
        const SkeletonLayout& layout,
        const BoneQts* locals,
        const BoneQts* objects,
        BoneQts* previousLocals,
        BoneQts* previousObjects,
        float translationTolerance,
        float rotationTolerance,
        std::uint32_t& checked,
        ChangedBoneCounts* counts)
    {
        checked = 0;
        std::vector<std::uint32_t> failing;
        const std::uint32_t boneCount = layout.boneCount;
        // Both blocks
        // byte-equal to the previous frame's (an idle copy): nothing
        // differs, nothing is checked and nothing is copied, which is where
        // the per-bone loops below end too.
        const std::size_t blockBytes = static_cast<std::size_t>(boneCount) * sizeof(BoneQts);
        const bool objectsEqual = std::memcmp(objects, previousObjects, blockBytes) == 0;
        const bool localsEqual = std::memcmp(locals, previousLocals, blockBytes) == 0;
        if (objectsEqual && localsEqual)
        {
            if (counts)
                *counts = ChangedBoneCounts{};
            return failing;
        }
        static thread_local std::vector<std::uint8_t> objectChanged;
        // The angle test for this tolerance (one tan per new
        // tolerance; the worker always passes the same one).
        static thread_local AngleTest angleTest;
        if (!angleTest.cheap || angleTest.epsilon != rotationTolerance)
            angleTest = MakeAngleTest(rotationTolerance);
        // The band of these tolerances, kept the same way.
        static thread_local ValidationBand band;
        if (FloatBits(band.translationTolerance) != FloatBits(translationTolerance) ||
            FloatBits(band.rotationTolerance) != FloatBits(rotationTolerance))
        {
            band = MakeValidationBand(translationTolerance, rotationTolerance);
        }
        objectChanged.resize(boneCount);
        std::uint32_t objectsChanged = 0;
        if (objectsEqual)
        {
            std::fill(objectChanged.begin(), objectChanged.end(), std::uint8_t{0});
        }
        else
        {
            // SSE instead of memcmp, here and below.
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                const bool differs = !SameBone(objects[bone], previousObjects[bone]);
                objectChanged[bone] = differs ? 1u : 0u;
                objectsChanged += differs ? 1u : 0u;
            }
        }
        const ExpectedObjectHook hook = g_expectedObjectHook.load(std::memory_order_relaxed);
        std::uint32_t localsChanged = 0;
        bool rootChanged = boneCount > 0 && objectChanged[0] != 0;
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            const std::int32_t parent = layout.parent[bone];
            const bool localChanged = !localsEqual && !SameBone(locals[bone], previousLocals[bone]);
            if (localChanged)
            {
                previousLocals[bone] = locals[bone];
                ++localsChanged;
                rootChanged = rootChanged || bone == 0;
            }
            if (parent == kUnresolvedParent)
                continue;
            const bool changed = objectChanged[bone] != 0 || localChanged ||
                (parent >= 0 && objectChanged[static_cast<std::uint32_t>(parent)] != 0);
            if (!changed)
                continue;
            ++checked;
            // A parented bone is decided from its SSE expected object when
            // that is clearly away from both tolerances; otherwise (and for a
            // root bone, whose expected object is its local) by the scalar
            // test below.
            if (parent >= 0 && band.usable)
            {
                const BoneQts& parentObject = objects[static_cast<std::uint32_t>(parent)];
                BoneQts composed;
                ComposeSimd(parentObject, locals[bone], layout.order, layout.inheritScale, composed);
                if (hook)
                    hook(composed, parentObject, bone);
                const int verdict = ValidationVerdict(band, parentObject, locals[bone], composed, objects[bone]);
                if (verdict != 0)
                {
                    if (verdict > 0)
                        failing.push_back(bone);
                    continue;
                }
            }
            const BoneQts expected = ExpectedObject(layout, bone, locals, objects);
            if (TranslationExceeds(expected, objects[bone], translationTolerance) ||
                AngleExceeds(angleTest, expected.rotation, objects[bone].rotation))
            {
                failing.push_back(bone);
            }
        }
        if (!objectsEqual)
        {
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
                if (objectChanged[bone] != 0)
                    previousObjects[bone] = objects[bone];
        }
        if (counts)
        {
            counts->locals = localsChanged;
            counts->objects = objectsChanged;
            counts->root = rootChanged;
        }
        return failing;
    }

    bool LayoutExplainsFrames(
        const SkeletonLayout& layout,
        const DiscoveryFrame* frames,
        std::uint32_t frameCount,
        std::uint32_t boneCount,
        const DiscoveryConfig& config)
    {
        if (!frames || frameCount == 0 || boneCount == 0 || layout.boneCount != boneCount ||
            layout.parent.size() != boneCount)
        {
            return false;
        }
        for (std::uint32_t frame = 0; frame < frameCount; ++frame)
        {
            if (!frames[frame].locals || !frames[frame].objects)
                return false;
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                if (layout.parent[bone] == kUnresolvedParent)
                    continue;
                float error = 0.0f;
                const BoneQts expected =
                    ExpectedObject(layout, bone, frames[frame].locals, frames[frame].objects);
                if (!Matches(expected, frames[frame].objects[bone], config, layout.inheritScale, error))
                    return false;
            }
        }
        return true;
    }

    DiscoveryStats DescribeLayout(const SkeletonLayout& layout)
    {
        DiscoveryStats stats;
        stats.order = layout.order;
        stats.inheritScale = layout.inheritScale;
        for (std::uint32_t bone = 0; bone < layout.boneCount && bone < layout.parent.size(); ++bone)
        {
            if (layout.parent[bone] == kRootParent)
                ++stats.roots;
            else if (layout.parent[bone] == kUnresolvedParent)
                ++stats.unresolved;
            else
                ++stats.parented;
            if (bone < layout.ambiguous.size() && layout.ambiguous[bone])
                ++stats.ambiguous;
            if (bone < layout.highPrecision.size() && layout.highPrecision[bone])
                ++stats.highPrecision;
        }
        return stats;
    }

    bool SameLayout(const SkeletonLayout& a, const SkeletonLayout& b)
    {
        return a.boneCount == b.boneCount && a.order == b.order &&
            a.inheritScale == b.inheritScale && a.parent == b.parent;
    }

    std::size_t PickReusableLayout(
        const SkeletonLayout* const* candidates,
        std::size_t count,
        const DiscoveryFrame* frames,
        std::uint32_t frameCount,
        std::uint32_t boneCount,
        const DiscoveryConfig& config)
    {
        std::size_t best = count;
        std::size_t bestUnresolved = 0;
        std::uint32_t bestParented = 0;
        for (std::size_t index = 0; index < count; ++index)
        {
            const SkeletonLayout* candidate = candidates[index];
            if (!candidate ||
                !LayoutExplainsFrames(*candidate, frames, frameCount, boneCount, config))
            {
                continue;
            }
            const std::size_t unresolved = candidate->unresolvedBones.size();
            const DiscoveryStats described = DescribeLayout(*candidate);
            if (best == count || unresolved < bestUnresolved ||
                (unresolved == bestUnresolved && described.parented > bestParented))
            {
                best = index;
                bestUnresolved = unresolved;
                bestParented = described.parented;
            }
        }
        return best;
    }

    namespace
    {
        // Whether `bone` is `ancestor` or one of its descendants.
        bool InSubtree(const SkeletonLayout& layout, std::uint32_t bone, std::uint32_t ancestor)
        {
            std::uint32_t steps = 0;
            std::int32_t current = static_cast<std::int32_t>(bone);
            while (current >= 0 && steps++ <= layout.boneCount)
            {
                if (static_cast<std::uint32_t>(current) == ancestor)
                    return true;
                current = layout.parent[static_cast<std::uint32_t>(current)];
            }
            return false;
        }
    }

    std::uint32_t ReparentBones(
        SkeletonLayout& layout,
        const std::vector<std::uint32_t>& bones,
        const DiscoveryFrame* frames,
        std::uint32_t frameCount,
        const DiscoveryConfig& config)
    {
        if (!frames || frameCount == 0 || layout.parent.size() != layout.boneCount)
            return 0;
        for (std::uint32_t frame = 0; frame < frameCount; ++frame)
        {
            if (!frames[frame].locals || !frames[frame].objects)
                return 0;
        }
        const Rule rule{layout.order, layout.inheritScale};
        std::uint32_t reparented = 0;
        for (const std::uint32_t bone : bones)
        {
            if (bone >= layout.boneCount || layout.parent[bone] != kUnresolvedParent)
                continue;
            std::vector<Candidate> candidates =
                FindCandidates(frames, frameCount, layout.boneCount, bone, rule, config);
            candidates.erase(
                std::remove_if(candidates.begin(), candidates.end(), [&](const Candidate& candidate) {
                    return candidate.parent >= 0 &&
                        InSubtree(layout, static_cast<std::uint32_t>(candidate.parent), bone);
                }),
                candidates.end());
            if (candidates.empty())
                continue;
            layout.parent[bone] = PickCandidate(candidates, bone).parent;
            if (bone < layout.ambiguous.size())
                layout.ambiguous[bone] = candidates.size() > 1 ? 1 : 0;
            ++reparented;
        }
        if (reparented)
            RebuildDerived(layout);
        return reparented;
    }

    void DemoteBones(SkeletonLayout& layout, const std::vector<std::uint32_t>& bones)
    {
        for (const std::uint32_t bone : bones)
        {
            if (bone < layout.boneCount)
                layout.parent[bone] = kUnresolvedParent;
        }
        RebuildDerived(layout);
    }

    LatePoseChange CompareLatePose(
        const BoneQts* finalizerLocals,
        const BoneQts* lateLocals,
        const BoneQts* lateObjects,
        const std::uint16_t* engineParents,
        std::uint32_t boneCount,
        float rootScale,
        float translationTolerance,
        float rotationTolerance,
        std::vector<BoneQts>& scratch)
    {
        LatePoseChange change;
        if (!finalizerLocals || !lateLocals || !lateObjects || !engineParents || boneCount == 0)
            return change;
        float worst = 0.0f;
        auto note = [&](std::uint32_t bone, float translation, float rotation) {
            change.maxTranslation = (std::max)(change.maxTranslation, translation);
            change.maxRotation = (std::max)(change.maxRotation, rotation);
            if (translation >= worst)
            {
                worst = translation;
                change.worstBone = static_cast<std::int32_t>(bone);
            }
        };
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            const float translation = TranslationDistance(finalizerLocals[bone], lateLocals[bone]);
            const float rotation = AngleBetween(finalizerLocals[bone].rotation, lateLocals[bone].rotation);
            if (translation > translationTolerance || rotation > rotationTolerance)
            {
                ++change.localBones;
                note(bone, translation, rotation);
            }
        }
        // The objects the finalizer would make from the end-of-frame locals,
        // each from the engine's own object of its parent (so one written
        // bone does not blame its descendants).
        scratch.resize(boneCount);
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            if (bone == 0)
            {
                scratch[0] = lateLocals[0];
                for (int axis = 0; axis < 3; ++axis)
                {
                    scratch[0].translation[axis] *= rootScale;
                    scratch[0].scale[axis] *= rootScale;
                }
                scratch[0].scale[3] *= rootScale;
            }
            else if (engineParents[bone] < bone)
            {
                scratch[bone] = Compose(lateObjects[engineParents[bone]], lateLocals[bone],
                    CompositionOrder::ParentThenLocal, true);
            }
            else
            {
                continue; // not composed by the finalizer either
            }
            const float translation = TranslationDistance(scratch[bone], lateObjects[bone]);
            const float rotation = AngleBetween(scratch[bone].rotation, lateObjects[bone].rotation);
            if (translation > translationTolerance || rotation > rotationTolerance)
            {
                ++change.objectBones;
                note(bone, translation, rotation);
            }
        }
        return change;
    }

    WrittenPoseChange CompareWrittenPose(
        const BoneQts* written,
        const BoneQts* now,
        std::uint32_t boneCount,
        float translationTolerance,
        float rotationTolerance)
    {
        WrittenPoseChange change;
        if (!written || !now || boneCount == 0)
            return change;
        float worst = -1.0f;
        for (std::uint32_t half = 0; half < 2; ++half)
        {
            const std::size_t base = static_cast<std::size_t>(half) * boneCount;
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                const float translation = TranslationDistance(written[base + bone], now[base + bone]);
                const float rotation = AngleBetween(written[base + bone].rotation, now[base + bone].rotation);
                if (!(translation > translationTolerance || rotation > rotationTolerance))
                    continue;
                ++(half == 0 ? change.localBones : change.objectBones);
                const std::int32_t index = static_cast<std::int32_t>(bone);
                if (change.firstBone < 0 || index < change.firstBone)
                    change.firstBone = index;
                change.lastBone = (std::max)(change.lastBone, index);
                change.maxTranslation = (std::max)(change.maxTranslation, translation);
                change.maxRotation = (std::max)(change.maxRotation, rotation);
                if (translation > worst)
                {
                    worst = translation;
                    change.worstBone = index;
                }
            }
        }
        return change;
    }
}
