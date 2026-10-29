#include "pose_stream.h"
#include "pose_capture_rules.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

// ---------------------------------------------------------------------------
// The scalar FindFailingBonesChanged (pose_skeleton_layout.cpp) as it was
// before the whole-block, SSE compare and SSE expected object version,
// unchanged apart from names: the reference that version is checked against
// (tests/pose_capture_tests.cpp uses it on recorded clips).
namespace v175
{
    using namespace replay::pose;

    namespace
    {
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
        static thread_local std::vector<std::uint8_t> objectChanged;
        // The angle test for this tolerance (one tan per new
        // tolerance; the worker always passes the same one).
        static thread_local AngleTest angleTest;
        if (!angleTest.cheap || angleTest.epsilon != rotationTolerance)
            angleTest = MakeAngleTest(rotationTolerance);
        objectChanged.resize(layout.boneCount);
        std::uint32_t objectsChanged = 0;
        for (std::uint32_t bone = 0; bone < layout.boneCount; ++bone)
        {
            const bool differs = std::memcmp(&objects[bone], &previousObjects[bone], sizeof(BoneQts)) != 0;
            objectChanged[bone] = differs ? 1u : 0u;
            objectsChanged += differs ? 1u : 0u;
        }
        std::uint32_t localsChanged = 0;
        bool rootChanged = layout.boneCount > 0 && objectChanged[0] != 0;
        for (std::uint32_t bone = 0; bone < layout.boneCount; ++bone)
        {
            const std::int32_t parent = layout.parent[bone];
            const bool localChanged = std::memcmp(&locals[bone], &previousLocals[bone], sizeof(BoneQts)) != 0;
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
            const BoneQts expected = ExpectedObject(layout, bone, locals, objects);
            if (TranslationExceeds(expected, objects[bone], translationTolerance) ||
                AngleExceeds(angleTest, expected.rotation, objects[bone].rotation))
            {
                failing.push_back(bone);
            }
        }
        for (std::uint32_t bone = 0; bone < layout.boneCount; ++bone)
            if (objectChanged[bone] != 0)
                previousObjects[bone] = objects[bone];
        if (counts)
        {
            counts->locals = localsChanged;
            counts->objects = objectsChanged;
            counts->root = rootChanged;
        }
        return failing;
    }
}

// Moves the SSE expected object as /fp:fast could, two
// ways: each rotation and translation lane by up to +-maxUlps of its own
// ulps, and by absolute amounts on the scale of the composition's rounding,
// which a lane far smaller than its operands (a cancelled translation, a
// small quaternion component) sees as thousands of its own ulps: each
// translation lane by up to +-translationUnits * 2^-24 * extent (|parent t|
// + |composed t - parent t|, L1, as ValidationVerdict measures it), each
// rotation lane by up to +-rotationUnits * 2^-24 (unit quaternions).
// Installed with SetExpectedObjectHook (tests/pose_capture_tests.cpp uses it
// too).
namespace band_nudge
{
    using namespace replay::pose;

    namespace
    {
        int g_maxUlps = 0;
        float g_translationUnits = 0.0f;
        float g_rotationUnits = 0.0f;
        std::mt19937 g_random(1);
    }

    // `steps` ulps up (positive) or down; finite values stay finite.
    float StepUlps(float value, int steps)
    {
        if (!std::isfinite(value))
            return value;
        std::int32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        // Integers in the order of the floats (-0 and +0 both 0).
        std::int64_t ordered = bits < 0 ? -static_cast<std::int64_t>(bits & 0x7FFFFFFF) : bits;
        ordered += steps;
        constexpr std::int64_t kLargest = 0x7F7FFFFF; // FLT_MAX
        ordered = std::min(std::max(ordered, -kLargest), kLargest);
        const std::uint32_t out = ordered < 0
            ? static_cast<std::uint32_t>(-ordered) | 0x80000000u
            : static_cast<std::uint32_t>(ordered);
        float result = 0.0f;
        std::memcpy(&result, &out, sizeof(result));
        return result;
    }

    void Nudge(BoneQts& composed, const BoneQts& parentObject, int maxUlps, float translationUnits,
        float rotationUnits, std::mt19937& random)
    {
        if (maxUlps > 0)
        {
            std::uniform_int_distribution<int> steps(-maxUlps, maxUlps);
            composed.rotation.x = StepUlps(composed.rotation.x, steps(random));
            composed.rotation.y = StepUlps(composed.rotation.y, steps(random));
            composed.rotation.z = StepUlps(composed.rotation.z, steps(random));
            composed.rotation.w = StepUlps(composed.rotation.w, steps(random));
            for (int axis = 0; axis < 3; ++axis)
                composed.translation[axis] = StepUlps(composed.translation[axis], steps(random));
        }
        if (translationUnits > 0.0f || rotationUnits > 0.0f)
        {
            constexpr float kUnit = 1.0f / 16777216.0f; // 2^-24
            std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
            float extent = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                extent += std::fabs(parentObject.translation[axis]) +
                    std::fabs(composed.translation[axis] - parentObject.translation[axis]);
            }
            if (std::isfinite(extent))
            {
                for (int axis = 0; axis < 3; ++axis)
                    composed.translation[axis] += unit(random) * translationUnits * kUnit * extent;
            }
            composed.rotation.x += unit(random) * rotationUnits * kUnit;
            composed.rotation.y += unit(random) * rotationUnits * kUnit;
            composed.rotation.z += unit(random) * rotationUnits * kUnit;
            composed.rotation.w += unit(random) * rotationUnits * kUnit;
        }
    }

    void Set(int maxUlps, float translationUnits, float rotationUnits, std::uint32_t seed)
    {
        g_maxUlps = maxUlps;
        g_translationUnits = translationUnits;
        g_rotationUnits = rotationUnits;
        g_random.seed(seed);
    }

    void Hook(BoneQts& composed, const BoneQts& parentObject, std::uint32_t)
    {
        Nudge(composed, parentObject, g_maxUlps, g_translationUnits, g_rotationUnits, g_random);
    }
}

namespace
{
    using namespace replay::pose;

    int g_poseFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_poseFailures;
    }

    Quat AxisAngle(float x, float y, float z, float angle)
    {
        const float length = std::sqrt(x * x + y * y + z * z);
        const float s = std::sin(angle * 0.5f) / length;
        return Normalize({x * s, y * s, z * s, std::cos(angle * 0.5f)});
    }

    // Synthetic metaped-like tree: a spine chain with two arms of finger
    // chains, plus independent "prop" bones that never move.
    struct SyntheticSkeleton
    {
        std::vector<std::int32_t> parent;
        std::vector<BoneQts> bind;

        SyntheticSkeleton()
        {
            auto add = [&](std::int32_t parentIndex, float tx, float ty, float tz) {
                BoneQts bone;
                bone.translation[0] = tx;
                bone.translation[1] = ty;
                bone.translation[2] = tz;
                bone.translation[3] = 0.04773f; // opaque engine value
                parent.push_back(parentIndex);
                bind.push_back(bone);
                return static_cast<std::int32_t>(parent.size() - 1);
            };
            const std::int32_t root = add(kRootParent, 0.0f, 0.0f, 0.0f);
            std::int32_t spine = root;
            for (int i = 0; i < 5; ++i)
                spine = add(spine, 0.0f, 0.0f, 0.12f);
            for (int side = -1; side <= 1; side += 2)
            {
                std::int32_t arm = add(spine, 0.18f * side, 0.0f, 0.0f);
                arm = add(arm, 0.28f * side, 0.0f, 0.0f);
                const std::int32_t hand = add(arm, 0.25f * side, 0.0f, 0.0f);
                for (int finger = 0; finger < 4; ++finger)
                {
                    std::int32_t joint = add(hand, 0.05f * side, 0.01f * finger, 0.0f);
                    joint = add(joint, 0.03f * side, 0.0f, 0.0f);
                    add(joint, 0.02f * side, 0.0f, 0.0f);
                }
            }
            for (int prop = 0; prop < 6; ++prop)
                add(root, 0.1f * prop, 0.2f, 1.0f);
        }

        std::uint32_t Count() const { return static_cast<std::uint32_t>(parent.size()); }

        // Pose at time t: spine and arms swing, fingers curl slowly, props rest.
        void Pose(
            float t,
            CompositionOrder order,
            std::vector<BoneQts>& locals,
            std::vector<BoneQts>& objects,
            bool scaledRoot = false) const
        {
            locals = bind;
            if (scaledRoot)
            {
                locals[0].scale[0] = 1.2f;
                locals[0].scale[1] = 0.9f;
                locals[0].scale[2] = 1.1f;
            }
            locals[0].rotation = AxisAngle(0.0f, 0.0f, 1.0f, 0.3f * t);
            locals[0].translation[0] = 1.5f * t;
            locals[0].translation[1] = 20.0f; // outside s16 range on purpose
            for (std::uint32_t bone = 1; bone < 6; ++bone)
                locals[bone].rotation = AxisAngle(1.0f, 0.0f, 0.0f, 0.1f * std::sin(t * 3.0f + bone));
            for (std::uint32_t bone = 6; bone < Count() - 6; ++bone)
                locals[bone].rotation = AxisAngle(0.0f, 1.0f, 0.2f, 0.4f * std::sin(t * 2.0f + 0.3f * bone));

            objects.assign(Count(), BoneQts{});
            for (std::uint32_t bone = 0; bone < Count(); ++bone)
            {
                objects[bone] = parent[bone] < 0
                    ? locals[bone]
                    : Compose(objects[static_cast<std::uint32_t>(parent[bone])], locals[bone], order, scaledRoot);
            }
        }
    };

    void TestQuaternionPacking()
    {
        std::mt19937 random(1234);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        float max32 = 0.0f;
        float max64 = 0.0f;
        for (int i = 0; i < 20000; ++i)
        {
            const Quat q = Normalize({unit(random), unit(random), unit(random), unit(random)});
            max32 = std::fmax(max32, AngleBetween(q, UnpackQuat32(PackQuat32(q))));
            max64 = std::fmax(max64, AngleBetween(q, UnpackQuat64(PackQuat64(q))));
        }
        Check(max32 < 0.004f, "quat32 smallest-three stays under 0.004 rad");
        Check(max64 < 0.0001f, "quat64 smallest-three stays under 0.0001 rad");

        const Quat negated{-0.2f, 0.1f, -0.3f, -0.9f};
        Check(AngleBetween(negated, UnpackQuat32(PackQuat32(negated))) < 0.004f,
            "quat32 treats q and -q as the same orientation");

        const float translation[3] = {1.25f, -3.5f, 0.001f};
        std::int16_t packed[3] = {};
        float unpacked[3] = {};
        Check(PackTranslation16(translation, 4.0f, packed),
            "translation inside range packs to s16");
        UnpackTranslation16(packed, 4.0f, unpacked);
        Check(std::fabs(unpacked[1] - translation[1]) < 0.0001f,
            "s16 translation keeps sub-millimetre precision");
        const float far[3] = {0.0f, 9.0f, 0.0f};
        Check(!PackTranslation16(far, 4.0f, packed),
            "translation outside range refuses s16 packing");
    }

    void TestLayoutDiscovery(CompositionOrder order, const char* label)
    {
        const SyntheticSkeleton skeleton;
        std::vector<BoneQts> locals[3];
        std::vector<BoneQts> objects[3];
        for (int frame = 0; frame < 3; ++frame)
            skeleton.Pose(0.4f * frame + 0.1f, order, locals[frame], objects[frame]);

        // A leaf prop gets an object transform no parent explains.
        const std::uint32_t odd = skeleton.Count() - 1;
        for (int frame = 0; frame < 3; ++frame)
        {
            objects[frame][odd].translation[2] += 0.5f + frame;
            objects[frame][odd].rotation = AxisAngle(0.3f, 1.0f, 0.0f, 0.7f + frame);
        }

        DiscoveryFrame frames[3];
        for (int frame = 0; frame < 3; ++frame)
            frames[frame] = {locals[frame].data(), objects[frame].data()};

        SkeletonLayout layout;
        DiscoveryStats stats;
        Check(DiscoverSkeletonLayout(frames, 3, skeleton.Count(), {}, layout, stats), label);
        Check(layout.order == order, "discovery selects the composition order");

        bool parentsMatch = true;
        for (std::uint32_t bone = 0; bone < skeleton.Count(); ++bone)
        {
            if (bone == odd)
                continue;
            if (layout.parent[bone] != skeleton.parent[bone])
                parentsMatch = false;
        }
        Check(parentsMatch, "discovery recovers every explainable parent");
        Check(layout.parent[odd] == kUnresolvedParent && stats.unresolved == 1,
            "an unexplainable bone stays unresolved");
        Check(layout.highPrecision[1] == 1 && layout.highPrecision[skeleton.Count() - 2] == 0,
            "large subtrees get 64-bit rotations, leaves do not");

        const CompositionError error =
            MeasureCompositionError(layout, locals[1].data(), objects[1].data());
        Check(error.maxTranslation < 0.0005f && error.maxRotation < 0.001f,
            "layout rebuilds engine object transforms");
    }

    struct Recorded
    {
        SyntheticSkeleton skeleton;
        SkeletonLayout layout;
        std::vector<std::vector<BoneQts>> locals;
        std::vector<std::vector<BoneQts>> objects;
    };

    Recorded MakeRecording(std::uint32_t frames)
    {
        Recorded recorded;
        recorded.locals.resize(frames);
        recorded.objects.resize(frames);
        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
            recorded.skeleton.Pose(
                frame / 30.0f,
                CompositionOrder::ParentThenLocal,
                recorded.locals[frame],
                recorded.objects[frame]);
        }
        DiscoveryFrame discovery[3] = {
            {recorded.locals[0].data(), recorded.objects[0].data()},
            {recorded.locals[5].data(), recorded.objects[5].data()},
            {recorded.locals[11].data(), recorded.objects[11].data()},
        };
        DiscoveryStats stats;
        DiscoverSkeletonLayout(discovery, 3, recorded.skeleton.Count(), {}, recorded.layout, stats);
        return recorded;
    }

    float MaxObjectError(const std::vector<BoneQts>& expected, const std::vector<BoneQts>& actual, float& rotation)
    {
        float translation = 0.0f;
        rotation = 0.0f;
        for (std::size_t bone = 0; bone < expected.size(); ++bone)
        {
            translation = std::fmax(translation, TranslationDistance(expected[bone], actual[bone]));
            rotation = std::fmax(rotation, AngleBetween(expected[bone].rotation, actual[bone].rotation));
        }
        return translation;
    }

    void TestStreamRoundTrip()
    {
        constexpr std::uint32_t kFrames = 150; // 5 s at 30 fps
        Recorded recorded = MakeRecording(kFrames);

        StreamConfig config;
        config.keyframeIntervalMs = 1000;
        PoseTrack track;
        track.Begin(recorded.layout, config);
        float world[16] = {};
        for (std::uint32_t frame = 0; frame < kFrames; ++frame)
        {
            world[12] = static_cast<float>(frame);
            FrameInput input;
            input.timeMs = frame * 33u;
            input.world = world;
            input.locals = recorded.locals[frame].data();
            input.objects = recorded.objects[frame].data();
            track.Append(input);
        }
        Check(track.Stats().frames == kFrames, "track stores every frame");
        Check(track.Stats().keyframes == 5, "one keyframe per interval");

        const std::size_t rawBytes = static_cast<std::size_t>(kFrames) *
            recorded.skeleton.Count() * sizeof(BoneQts) * 2u;
        Check(track.Stats().bytes * 4u < rawBytes,
            "stream is at least 4x smaller than raw local+object copies");

        // Exact frame times, forward then backwards (seek/rewind).
        float worstTranslation = 0.0f;
        float worstRotation = 0.0f;
        bool sampled = true;
        PoseSample sample;
        for (int pass = 0; pass < 2; ++pass)
        {
            for (std::uint32_t step = 0; step < kFrames; ++step)
            {
                const std::uint32_t frame = pass == 0 ? step : kFrames - 1 - step;
                sampled = sampled && track.Sample(frame * 33u, sample);
                float rotation = 0.0f;
                worstTranslation = std::fmax(
                    worstTranslation,
                    MaxObjectError(recorded.objects[frame], sample.objects, rotation));
                worstRotation = std::fmax(worstRotation, rotation);
            }
        }
        Check(sampled, "every recorded frame samples forward and backwards");
        Check(worstTranslation < 0.01f,
            "reconstructed object translation within 1 cm across a 6-bone chain");
        Check(worstRotation < 0.02f, "reconstructed object rotation within 0.02 rad");

        Check(track.Sample(10u * 33u + 16u, sample) && sample.t > 0.4f && sample.t < 0.6f &&
                std::fabs(sample.worldA[12] - 10.0f) < 1e-6f &&
                std::fabs(sample.worldB[12] - 11.0f) < 1e-6f,
            "sampling between frames interpolates with the frame pair");
    }

    // Sample cache: a track sampled at 60 fps playback steps,
    // forwards, across chunk boundaries and with seeks, returns exactly what
    // a track that never sampled before returns for the same time.
    void TestSampleCacheMatchesFreshDecode()
    {
        constexpr std::uint32_t kFrames = 120;
        Recorded recorded = MakeRecording(kFrames);
        StreamConfig config;
        config.keyframeIntervalMs = 500;
        PoseTrack pristine;
        pristine.Begin(recorded.layout, config);
        float world[16] = {};
        for (std::uint32_t frame = 0; frame < kFrames; ++frame)
        {
            world[12] = static_cast<float>(frame);
            FrameInput input;
            input.timeMs = frame * 33u;
            input.flags = frame == 40 ? kFrameNoInterpolation : 0;
            input.world = world;
            input.locals = recorded.locals[frame].data();
            input.objects = recorded.objects[frame].data();
            pristine.Append(input);
        }

        std::vector<std::uint32_t> times;
        for (std::uint32_t time = 0; time <= (kFrames - 1) * 33u; time += 16u)
            times.push_back(time);
        for (std::uint32_t time : {3000u, 100u, 1650u, 1651u, 1683u, 16u, 3900u, 3901u, 500u, 499u})
            times.push_back(time);

        PoseTrack cached = pristine;
        bool same = true;
        PoseSample a;
        PoseSample b;
        for (const std::uint32_t time : times)
        {
            PoseTrack fresh = pristine;
            const bool okA = cached.Sample(time, a);
            const bool okB = fresh.Sample(time, b);
            same = same && okA && okB && a.t == b.t && a.frameTimeA == b.frameTimeA &&
                a.frameTimeB == b.frameTimeB &&
                a.locals.size() == b.locals.size() && a.objects.size() == b.objects.size() &&
                std::memcmp(a.locals.data(), b.locals.data(), a.locals.size() * sizeof(BoneQts)) == 0 &&
                std::memcmp(a.objects.data(), b.objects.data(), a.objects.size() * sizeof(BoneQts)) == 0 &&
                std::memcmp(a.worldA, b.worldA, sizeof(a.worldA)) == 0 &&
                std::memcmp(a.worldB, b.worldB, sizeof(a.worldB)) == 0;
        }
        Check(same, "cached sampling equals a fresh decode at every playback step and seek");
    }

    // Backward scrub from checkpoints. A track scrubbed backward
    // (2x and fine steps, across chunk boundaries, with a forward stretch and
    // a seek in the middle) returns bit for bit what the same track returns
    // without checkpoints, and decodes far fewer frames.
    void TestBackwardScrubCheckpoints()
    {
        constexpr std::uint32_t kFrames = 200; // 6.6 s at 30 fps, 2 s chunks with relative keyframes
        Recorded recorded = MakeRecording(kFrames);
        StreamConfig config;
        PoseTrack pristine;
        pristine.Begin(recorded.layout, config);
        float world[16] = {};
        for (std::uint32_t frame = 0; frame < kFrames; ++frame)
        {
            world[12] = static_cast<float>(frame);
            world[13] = static_cast<float>(frame % 7) * 0.25f;
            FrameInput input;
            input.timeMs = frame * 33u;
            input.flags = frame == 97 ? kFrameNoInterpolation : 0;
            input.world = world;
            input.locals = recorded.locals[frame].data();
            input.objects = recorded.objects[frame].data();
            pristine.Append(input);
        }
        Check(pristine.Stats().relativeKeyframes >= 2, "checkpoint track has relative keyframes");

        std::vector<std::uint32_t> times;
        const std::uint32_t last = (kFrames - 1) * 33u;
        for (std::uint32_t time = last; time >= 40u; time -= 33u) // 2x scrub back at 60 fps
            times.push_back(time);
        for (std::uint32_t time = 0; time <= 1500u; time += 16u) // normal play forward
            times.push_back(time);
        for (std::uint32_t time = 1500u; time >= 900u; time -= 10u) // fine scrub back
            times.push_back(time);
        times.push_back(5000u); // a seek, then back again
        for (std::uint32_t time = 5000u; time >= 3000u; time -= 33u)
            times.push_back(time);

        // Where the chunks start: sampled forward frame by frame, only the
        // last frame of a chunk decodes (the next chunk's first frame).
        std::vector<std::uint32_t> chunkStarts{0};
        {
            PoseTrack probe = pristine;
            PoseSample unused;
            for (std::uint32_t frame = 0; frame + 1 < kFrames; ++frame)
            {
                const std::uint64_t before = DecodedFrameCount();
                probe.Sample(frame * 33u, unused);
                if (frame > 0 && DecodedFrameCount() - before == 1)
                    chunkStarts.push_back((frame + 1) * 33u);
            }
        }
        const auto chunkOf = [&](std::uint32_t time) {
            return static_cast<std::size_t>(std::upper_bound(chunkStarts.begin(), chunkStarts.end(), time) - chunkStarts.begin());
        };
        Check(chunkStarts.size() == 4, "the checkpoint track has four chunks");

        PoseTrack withCheckpoints = pristine;
        PoseTrack plain = pristine;
        bool same = true;
        std::uint64_t decodedWith = 0;
        std::uint64_t decodedPlain = 0;
        std::uint64_t worstTickWith = 0;
        PoseSample a;
        PoseSample b;
        std::uint32_t previous = ~0u;
        for (const std::uint32_t time : times)
        {
            PoseTrack::SetCheckpointsEnabled(true);
            const std::uint64_t before = DecodedFrameCount();
            const bool okA = withCheckpoints.Sample(time, a);
            const std::uint64_t tick = DecodedFrameCount() - before;
            decodedWith += tick;
            // After the first decode of a chunk, a backward step decodes at
            // most a stride of frames (plus the next frame's lookahead).
            if (previous != ~0u && time < previous && chunkOf(time) == chunkOf(previous))
                worstTickWith = std::max(worstTickWith, tick);
            PoseTrack::SetCheckpointsEnabled(false);
            const std::uint64_t beforePlain = DecodedFrameCount();
            const bool okB = plain.Sample(time, b);
            decodedPlain += DecodedFrameCount() - beforePlain;
            PoseTrack::SetCheckpointsEnabled(true);
            same = same && okA && okB && a.t == b.t && a.frameTimeA == b.frameTimeA &&
                a.frameTimeB == b.frameTimeB &&
                a.locals.size() == b.locals.size() && a.objects.size() == b.objects.size() &&
                std::memcmp(a.locals.data(), b.locals.data(), a.locals.size() * sizeof(BoneQts)) == 0 &&
                std::memcmp(a.objects.data(), b.objects.data(), a.objects.size() * sizeof(BoneQts)) == 0 &&
                std::memcmp(a.worldA, b.worldA, sizeof(a.worldA)) == 0 &&
                std::memcmp(a.worldB, b.worldB, sizeof(a.worldB)) == 0;
            previous = time;
        }
        Check(same, "backward scrub from checkpoints equals the decode from the chunk keyframe, bit for bit");
        Check(withCheckpoints.CheckpointRestores() > 0, "backward scrub starts from checkpoints");
        Check(worstTickWith <= 9, "a backward step inside a chunk decodes at most a checkpoint stride");
        Check(decodedWith * 3 < decodedPlain, "checkpoints cut the frames decoded by a backward scrub by more than 3x");
        Check(withCheckpoints.CheckpointBytes() > 0 && plain.CheckpointBytes() == 0,
            "only the scrubbed track holds checkpoints");
        std::printf("  backward scrub decodes: %llu with checkpoints, %llu without, worst tick %llu\n",
            static_cast<unsigned long long>(decodedWith), static_cast<unsigned long long>(decodedPlain),
            static_cast<unsigned long long>(worstTickWith));

        // Normal play two chunks on frees them.
        for (std::uint32_t time = 3000u; time <= last; time += 16u)
            withCheckpoints.Sample(time, a);
        Check(withCheckpoints.CheckpointBytes() == 0, "playing on two chunks frees the checkpoints");
    }

    // Layout reuse: a layout discovered on one ped explains the
    // frames of another ped of the same skeleton (other motion), and is
    // refused when a parent is wrong or the bone count differs.
    void TestLayoutReuseAcrossPeds()
    {
        const SyntheticSkeleton skeleton;
        std::vector<BoneQts> locals[6];
        std::vector<BoneQts> objects[6];
        for (int frame = 0; frame < 6; ++frame)
            skeleton.Pose(0.3f * frame + (frame < 3 ? 0.1f : 5.0f), CompositionOrder::ParentThenLocal,
                locals[frame], objects[frame]);
        DiscoveryFrame donor[3];
        DiscoveryFrame other[3];
        for (int frame = 0; frame < 3; ++frame)
        {
            donor[frame] = {locals[frame].data(), objects[frame].data()};
            other[frame] = {locals[frame + 3].data(), objects[frame + 3].data()};
        }
        SkeletonLayout layout;
        DiscoveryStats stats;
        Check(DiscoverSkeletonLayout(donor, 3, skeleton.Count(), {}, layout, stats),
            "donor layout discovered");
        Check(LayoutExplainsFrames(layout, other, 3, skeleton.Count(), {}),
            "a donor layout explains another ped of the same skeleton");
        const DiscoveryStats described = DescribeLayout(layout);
        Check(described.roots == stats.roots && described.parented == stats.parented &&
                described.unresolved == stats.unresolved && described.highPrecision == stats.highPrecision,
            "a reused layout reports the same counts as its discovery");

        SkeletonLayout wrong = layout;
        const std::uint32_t finger = skeleton.Count() - 8;
        wrong.parent[finger] = 0;
        Check(!LayoutExplainsFrames(wrong, other, 3, skeleton.Count(), {}),
            "a layout with a wrong parent is refused");
        SkeletonLayout demoted = wrong;
        DemoteBones(demoted, {finger});
        Check(LayoutExplainsFrames(demoted, other, 3, skeleton.Count(), {}),
            "an object-space bone always explains its frames");
        Check(!LayoutExplainsFrames(layout, other, 3, skeleton.Count() + 1, {}),
            "another bone count is refused");

        // The demoted layout explains the frames too, but the
        // clean one is picked whatever the order; a wrong one is skipped.
        const SkeletonLayout* candidates[] = {&demoted, &wrong, &layout};
        Check(PickReusableLayout(candidates, 3, other, 3, skeleton.Count(), {}) == 2,
            "the candidate with the fewest object-space bones is picked");
        const SkeletonLayout* onlyDemoted[] = {&wrong, &demoted};
        Check(PickReusableLayout(onlyDemoted, 2, other, 3, skeleton.Count(), {}) == 1,
            "a demoted layout is still picked when nothing better explains the frames");
        const SkeletonLayout* none[] = {&wrong};
        Check(PickReusableLayout(none, 1, other, 3, skeleton.Count(), {}) == 1,
            "no candidate: count is returned");
        Check(SameLayout(layout, layout) && !SameLayout(layout, demoted) && !SameLayout(layout, wrong),
            "SameLayout compares bone count, composition and parents");
    }

    // One frame through the reference FindFailingBonesChanged (v175) and the
    // current one, each on its own copy of the same previous arrays: the same
    // failing bones, checked count, ChangedBoneCounts (when asked) and
    // previous arrays afterwards.
    struct ChangedCheckTally
    {
        std::uint64_t frames = 0;
        std::uint64_t checked = 0;
        std::uint64_t failing = 0;
        std::uint64_t differ = 0;
    };

    bool SameChangedCheck(
        const SkeletonLayout& layout,
        const BoneQts* locals,
        const BoneQts* objects,
        const BoneQts* previousLocals,
        const BoneQts* previousObjects,
        float translationTolerance,
        float rotationTolerance,
        ChangedCheckTally& tally,
        bool withCounts = true)
    {
        const std::size_t count = layout.boneCount;
        std::vector<BoneQts> oldLocals(previousLocals, previousLocals + count);
        std::vector<BoneQts> oldObjects(previousObjects, previousObjects + count);
        std::vector<BoneQts> newLocals = oldLocals;
        std::vector<BoneQts> newObjects = oldObjects;
        std::uint32_t oldChecked = 77;
        std::uint32_t newChecked = 99;
        ChangedBoneCounts oldCounts{11, 12, false};
        ChangedBoneCounts newCounts{13, 14, true};
        const std::vector<std::uint32_t> oldFailing = v175::FindFailingBonesChanged(layout, locals, objects,
            oldLocals.data(), oldObjects.data(), translationTolerance, rotationTolerance, oldChecked,
            withCounts ? &oldCounts : nullptr);
        const std::vector<std::uint32_t> newFailing = FindFailingBonesChanged(layout, locals, objects,
            newLocals.data(), newObjects.data(), translationTolerance, rotationTolerance, newChecked,
            withCounts ? &newCounts : nullptr);
        const bool sameCounts = !withCounts || (oldCounts.locals == newCounts.locals &&
            oldCounts.objects == newCounts.objects && oldCounts.root == newCounts.root);
        const bool sameArrays = count == 0 ||
            (std::memcmp(oldLocals.data(), newLocals.data(), count * sizeof(BoneQts)) == 0 &&
                std::memcmp(oldObjects.data(), newObjects.data(), count * sizeof(BoneQts)) == 0);
        const bool same = oldFailing == newFailing && oldChecked == newChecked && sameCounts && sameArrays;
        ++tally.frames;
        tally.checked += oldChecked;
        tally.failing += oldFailing.size();
        tally.differ += same ? 0u : 1u;
        return same;
    }

    // The changed-bones check gives FindFailingBones' result on frames after
    // one that passed: unchanged bones, moved bones that still agree, broken
    // objects, broken locals, parents that moved alone. And every frame gives
    // the reference check's result (SameChangedCheck).
    void TestFailingBonesChanged()
    {
        ChangedCheckTally reference;
        const SyntheticSkeleton skeleton;
        std::vector<BoneQts> locals[4];
        std::vector<BoneQts> objects[4];
        DiscoveryFrame frames[4];
        for (int frame = 0; frame < 4; ++frame)
        {
            skeleton.Pose(0.3f * frame + 0.2f, CompositionOrder::ParentThenLocal, locals[frame], objects[frame]);
            frames[frame] = {locals[frame].data(), objects[frame].data()};
        }
        SkeletonLayout layout;
        DiscoveryStats stats;
        Check(DiscoverSkeletonLayout(frames, 3, skeleton.Count(), {}, layout, stats), "changed bones: layout discovered");
        const std::uint32_t count = skeleton.Count();
        Check(FindFailingBones(layout, locals[0].data(), objects[0].data(), 0.002f, 0.004f).empty(),
            "changed bones: the previous frame passes");
        std::mt19937 random(4242);
        std::uniform_int_distribution<std::uint32_t> pick(0, count - 1);
        std::uniform_real_distribution<float> small(-0.01f, 0.01f);
        int mismatches = 0;
        std::uint64_t checkedTotal = 0;
        for (int trial = 0; trial < 400; ++trial)
        {
            std::vector<BoneQts> l = locals[0];
            std::vector<BoneQts> o = objects[0];
            const int kind = trial % 5;
            if (kind == 1) // a whole new pose that agrees
            {
                l = locals[1 + trial % 3];
                o = objects[1 + trial % 3];
            }
            const int edits = 1 + static_cast<int>(pick(random) % 6);
            for (int e = 0; e < edits; ++e)
            {
                const std::uint32_t bone = pick(random);
                if (kind == 2)
                    o[bone].translation[static_cast<int>(pick(random) % 3)] += small(random) * 50.0f; // broken object
                else if (kind == 3)
                    l[bone].translation[static_cast<int>(pick(random) % 3)] += small(random) * 50.0f; // broken local
                else if (kind == 4)
                    o[bone].rotation.w += small(random) * 10.0f; // parent object moved alone
                else if (kind == 0)
                    o[bone].translation[3] += 1.0f; // an opaque lane only
            }
            std::uint32_t checked = 0;
            std::vector<BoneQts> previousLocals = locals[0];
            std::vector<BoneQts> previousObjects = objects[0];
            SameChangedCheck(layout, l.data(), o.data(), locals[0].data(), objects[0].data(), 0.002f, 0.004f,
                reference, trial % 2 == 0);
            const std::vector<std::uint32_t> full = FindFailingBones(layout, l.data(), o.data(), 0.002f, 0.004f);
            const std::vector<std::uint32_t> changed = FindFailingBonesChanged(
                layout, l.data(), o.data(), previousLocals.data(), previousObjects.data(), 0.002f, 0.004f, checked);
            mismatches += full != changed ? 1 : 0;
            // The previous arrays now hold this frame.
            mismatches += std::memcmp(previousLocals.data(), l.data(), l.size() * sizeof(BoneQts)) != 0 ||
                    std::memcmp(previousObjects.data(), o.data(), o.size() * sizeof(BoneQts)) != 0
                ? 1
                : 0;
            checkedTotal += checked;
        }
        Check(mismatches == 0, "changed bones: same failing bones as the full check in every trial");
        std::uint32_t checked = 99;
        std::vector<BoneQts> sameLocals = locals[0];
        std::vector<BoneQts> sameObjects = objects[0];
        Check(FindFailingBonesChanged(layout, locals[0].data(), objects[0].data(), sameLocals.data(), sameObjects.data(),
                  0.002f, 0.004f, checked).empty() && checked == 0,
            "changed bones: an identical frame recomputes nothing");
        Check(checkedTotal < 400ull * count, "changed bones: fewer bones recomputed than the full check");

        // The tolerance tests are the cheap ones (TranslationExceeds,
        // AngleExceeds); objects moved right at the tolerances (2 mm, 0.004
        // rad, a few ulps either side) fail exactly the bones the full check
        // with TranslationDistance and AngleBetween fails.
        int nearMismatches = 0;
        int nearPassing = 0;
        int nearFailing = 0;
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        std::uniform_int_distribution<int> ulps(-6, 6);
        for (int trial = 0; trial < 4000; ++trial)
        {
            std::vector<BoneQts> l = locals[1 + trial % 3];
            std::vector<BoneQts> o = objects[1 + trial % 3];
            const std::uint32_t bone = 1 + pick(random) % (count - 1);
            if (trial % 2 == 0)
            {
                // Translation offset of length 0.002 (stepped by ulps).
                float dir[3] = {unit(random), unit(random), unit(random)};
                const float length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                if (!(length > 0.1f))
                    continue;
                float target = 0.002f;
                const int step = ulps(random);
                for (int i = 0; i < (step < 0 ? -step : step); ++i)
                    target = std::nextafter(target, step < 0 ? 0.0f : 1.0f);
                for (int axis = 0; axis < 3; ++axis)
                    o[bone].translation[axis] += dir[axis] / length * target;
            }
            else
            {
                // Rotation by 0.004 rad (stepped) about a random axis.
                float axisV[3] = {unit(random), unit(random), unit(random)};
                const float length = std::sqrt(axisV[0] * axisV[0] + axisV[1] * axisV[1] + axisV[2] * axisV[2]);
                if (!(length > 0.1f))
                    continue;
                float angle = 0.004f;
                const int step = ulps(random);
                for (int i = 0; i < (step < 0 ? -step : step) * 64; ++i)
                    angle = std::nextafter(angle, step < 0 ? 0.0f : 1.0f);
                const float half = 0.5f * angle;
                const Quat delta{axisV[0] / length * std::sin(half), axisV[1] / length * std::sin(half),
                    axisV[2] / length * std::sin(half), std::cos(half)};
                o[bone].rotation = Multiply(o[bone].rotation, delta);
            }
            std::vector<BoneQts> previousLocals = locals[1 + trial % 3];
            std::vector<BoneQts> previousObjects = objects[1 + trial % 3];
            SameChangedCheck(layout, l.data(), o.data(), previousLocals.data(), previousObjects.data(), 0.002f, 0.004f,
                reference);
            std::uint32_t nearChecked = 0;
            ChangedBoneCounts counts;
            const std::vector<std::uint32_t> full = FindFailingBones(layout, l.data(), o.data(), 0.002f, 0.004f);
            const std::vector<std::uint32_t> changed = FindFailingBonesChanged(
                layout, l.data(), o.data(), previousLocals.data(), previousObjects.data(), 0.002f, 0.004f, nearChecked,
                &counts);
            nearMismatches += full != changed ? 1 : 0;
            nearMismatches += counts.locals != 0 || counts.objects != 1 || counts.root ? 1 : 0;
            nearFailing += full.empty() ? 0 : 1;
            nearPassing += full.empty() ? 1 : 0;
        }
        Check(nearMismatches == 0, "changed bones: cheap tolerance tests give the full check's bones at the threshold");
        Check(nearFailing > 0 && nearPassing > 0, "changed bones: threshold trials fall on both sides");
        std::printf("  changed bones against reference: %llu frames (400 edited, the rest on the tolerances), %llu bones "
                    "checked, %llu failing, %llu differ\n",
            static_cast<unsigned long long>(reference.frames), static_cast<unsigned long long>(reference.checked),
            static_cast<unsigned long long>(reference.failing), static_cast<unsigned long long>(reference.differ));
        Check(reference.differ == 0 && reference.frames > 4000,
            "changed bones: failing bones, checked count, counts and previous arrays of the reference check");

        // The counts of what differed (evidence of the script reads).
        {
            std::vector<BoneQts> l = locals[0];
            std::vector<BoneQts> o = objects[0];
            l[0].translation[3] += 1.0f; // root local, opaque lane
            o[3].translation[3] += 1.0f;
            o[5].translation[3] += 1.0f;
            std::vector<BoneQts> previousLocals = locals[0];
            std::vector<BoneQts> previousObjects = objects[0];
            std::uint32_t countChecked = 0;
            ChangedBoneCounts counts;
            FindFailingBonesChanged(layout, l.data(), o.data(), previousLocals.data(), previousObjects.data(), 0.002f,
                0.004f, countChecked, &counts);
            Check(counts.locals == 1 && counts.objects == 2 && counts.root,
                "changed bones: counts of differing locals, objects and the root");
            ChangedBoneCounts same;
            FindFailingBonesChanged(layout, l.data(), o.data(), previousLocals.data(), previousObjects.data(), 0.002f,
                0.004f, countChecked, &same);
            Check(same.locals == 0 && same.objects == 0 && !same.root && countChecked == 0,
                "changed bones: an identical frame counts nothing");
        }

        // Cost per recomputed bone, full check (TranslationDistance,
        // AngleBetween) against the changed-bones check on a frame where every
        // bone moved (cheap tests, plus its byte compares and copies).
        {
            constexpr int kRounds = 20000;
            std::vector<BoneQts> previousLocals;
            std::vector<BoneQts> previousObjects;
            std::size_t sink = 0;
            const auto fullStart = std::chrono::steady_clock::now();
            for (int round = 0; round < kRounds; ++round)
                sink += FindFailingBones(layout, locals[1 + round % 3].data(), objects[1 + round % 3].data(), 0.002f,
                    0.004f).size();
            const auto fullEnd = std::chrono::steady_clock::now();
            std::uint64_t recomputed = 0;
            double changedNs = 0.0;
            // The reference check on the same frames, the two taking turns
            // going first.
            double referenceNs = 0.0;
            for (int round = 0; round < kRounds; ++round)
            {
                for (int pass = 0; pass < 2; ++pass)
                {
                    const bool old = (round + pass) % 2 == 0;
                    previousLocals = locals[round % 3 == 0 ? 2 : 0];
                    previousObjects = objects[round % 3 == 0 ? 2 : 0];
                    std::uint32_t benchChecked = 0;
                    const auto start = std::chrono::steady_clock::now();
                    if (old)
                        sink += v175::FindFailingBonesChanged(layout, locals[1 + round % 3].data(),
                            objects[1 + round % 3].data(), previousLocals.data(), previousObjects.data(), 0.002f, 0.004f,
                            benchChecked, nullptr).size();
                    else
                        sink += FindFailingBonesChanged(layout, locals[1 + round % 3].data(),
                            objects[1 + round % 3].data(), previousLocals.data(), previousObjects.data(), 0.002f, 0.004f,
                            benchChecked).size();
                    const double ns =
                        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
                    if (old)
                    {
                        referenceNs += ns;
                    }
                    else
                    {
                        changedNs += ns;
                        recomputed += benchChecked;
                    }
                }
            }
            const double fullNs = std::chrono::duration<double, std::nano>(fullEnd - fullStart).count();
            std::printf("  validation per recomputed bone (%u bones): full check %.1f ns, changed-bones check "
                        "reference %.1f ns -> current %.1f ns (%.0f%% of the bones recomputed, sink %zu)\n",
                count, fullNs / (static_cast<double>(kRounds) * count),
                recomputed ? referenceNs / static_cast<double>(recomputed) : 0.0,
                recomputed ? changedNs / static_cast<double>(recomputed) : 0.0,
                100.0 * static_cast<double>(recomputed) / (static_cast<double>(kRounds) * count), sink);
        }
    }

    // ---------------------------------------------------------------------
    // Random layouts (both composition orders, inherited scale on and off,
    // roots, object- space bones, parents before and after their children),
    // poses from a tenth of a millimetre to tens of metres, and frames edited
    // every way the worker can see them; FindFailingBonesChanged against the
    // reference one, and ValidationVerdict bone by bone against the scalar
    // test, without and with the SSE expected object moved by +-32 ulps per
    // lane (what /fp:fast could do, many times over).
    SkeletonLayout RandomValidationLayout(
        std::mt19937& random, std::uint32_t boneCount, CompositionOrder order, bool inheritScale)
    {
        SkeletonLayout layout;
        layout.boneCount = boneCount;
        layout.order = order;
        layout.inheritScale = inheritScale;
        layout.parent.assign(boneCount, kUnresolvedParent);
        layout.ambiguous.assign(boneCount, 0);
        layout.highPrecision.assign(boneCount, 0);
        for (std::uint32_t bone = 0; bone < boneCount; ++bone)
        {
            const std::uint32_t pick = static_cast<std::uint32_t>(random() % 100u);
            std::int32_t parent = kRootParent;
            if (pick < 5 || bone == 0)
                parent = pick < 50 ? kRootParent : kUnresolvedParent;
            else if (pick < 12)
                parent = kUnresolvedParent;
            else if (pick < 16 && bone + 1 < boneCount)
                parent = static_cast<std::int32_t>(bone + 1 + random() % (boneCount - bone - 1)); // a later bone
            else if (pick < 70)
                parent = static_cast<std::int32_t>(bone - 1);
            else
                parent = static_cast<std::int32_t>(random() % bone);
            layout.parent[bone] = parent;
        }
        DemoteBones(layout, {}); // evaluation order, object-space list (a cycle demotes a bone)
        return layout;
    }

    float RandomLength(std::mt19937& random, bool wide)
    {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const float pick = unit(random);
        if (pick < 0.08f)
            return 0.0f;
        // 1e-4 to 40 m, or 2 cm to 40 cm (engine-like bones).
        const float magnitude = wide ? std::pow(10.0f, -4.0f + 5.6f * unit(random)) : 0.02f + 0.38f * unit(random);
        return pick < 0.54f ? magnitude : -magnitude;
    }

    BoneQts RandomValidationBone(std::mt19937& random, bool scaled, bool wide)
    {
        std::normal_distribution<float> normal(0.0f, 1.0f);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        BoneQts bone;
        bone.rotation = Normalize({normal(random), normal(random), normal(random), normal(random)});
        for (int axis = 0; axis < 3; ++axis)
            bone.translation[axis] = RandomLength(random, wide);
        bone.translation[3] = unit(random);
        if (scaled && unit(random) < 0.7f)
        {
            for (int axis = 0; axis < 3; ++axis)
                bone.scale[axis] = 0.6f + unit(random);
        }
        bone.scale[3] = unit(random) < 0.5f ? 1.0f : unit(random);
        return bone;
    }

    // Objects composed with the layout from the locals (object-space bones
    // keep `objects`' own, or a random one when there is none).
    void ComposeValidationObjects(std::mt19937& random, const SkeletonLayout& layout, bool wide,
        const std::vector<BoneQts>& locals, std::vector<BoneQts>& objects)
    {
        std::vector<BoneQts> unresolved;
        for (const std::uint32_t bone : layout.unresolvedBones)
        {
            unresolved.push_back(bone < objects.size() ? objects[bone]
                                                       : RandomValidationBone(random, layout.inheritScale, wide));
        }
        objects.resize(layout.boneCount);
        if (layout.boneCount > 0)
            ComposeObjects(layout, locals.data(), unresolved.data(), objects.data());
    }

    void RandomValidationPose(std::mt19937& random, const SkeletonLayout& layout, bool wide,
        std::vector<BoneQts>& locals, std::vector<BoneQts>& objects)
    {
        locals.resize(layout.boneCount);
        for (BoneQts& bone : locals)
            bone = RandomValidationBone(random, layout.inheritScale, wide);
        objects.clear();
        ComposeValidationObjects(random, layout, wide, locals, objects);
    }

    // `bone`'s object onto a tolerance: its scalar expected object (the
    // parent's current object composed with the local) with the translation
    // moved by the translation tolerance or the rotation turned by the
    // rotation tolerance, a few ulps either side (64 ulps per step for the
    // angle), or with `spread` the tolerance
    // times 1 +- up to `spread` (across the band's edges).
    void PlaceOnTolerance(std::mt19937& random, const SkeletonLayout& layout, const std::vector<BoneQts>& locals,
        std::vector<BoneQts>& objects, std::uint32_t bone, float translationTolerance, float rotationTolerance,
        float spread = 0.0f)
    {
        const std::int32_t parent = layout.parent[bone];
        if (parent == kUnresolvedParent)
            return;
        const BoneQts expected = parent == kRootParent
            ? locals[bone]
            : Compose(objects[static_cast<std::uint32_t>(parent)], locals[bone], layout.order, layout.inheritScale);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        std::uniform_int_distribution<int> ulps(-12, 12);
        float direction[3] = {unit(random), unit(random), unit(random)};
        float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
        if (!(length > 0.1f))
        {
            direction[0] = 1.0f;
            length = 1.0f;
        }
        BoneQts& object = objects[bone];
        object = expected;
        const float factor = 1.0f + spread * unit(random);
        if (random() % 2u == 0)
        {
            const float target = spread > 0.0f ? translationTolerance * factor
                                               : band_nudge::StepUlps(translationTolerance, ulps(random));
            for (int axis = 0; axis < 3; ++axis)
                object.translation[axis] = expected.translation[axis] + direction[axis] / length * target;
        }
        else
        {
            const float angle = spread > 0.0f ? rotationTolerance * factor
                                              : band_nudge::StepUlps(rotationTolerance, ulps(random) * 64);
            object.rotation = Multiply(expected.rotation, AxisAngle(direction[0], direction[1], direction[2], angle));
        }
    }

    // NaN, infinities, huge, tiny and denormal values in one lane, or a
    // rotation far from unit (squared norms just inside and outside the
    // band's (0.5, 2), zero, tiny, large).
    void InjectOddValue(std::mt19937& random, BoneQts& bone)
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();
        const float values[] = {nan, -nan, infinity, -infinity, 1e30f, -1e30f, std::numeric_limits<float>::max(),
            std::numeric_limits<float>::min() * 0.01f, -0.0f, 3e5f, 2e6f, 1e-30f};
        if (random() % 4u == 0)
        {
            const float scales[] = {0.0f, 1e-20f, 0.3f, 0.7f, 0.72f, 1.4f, 1.42f, 3.0f};
            const float s = scales[random() % 8u];
            bone.rotation = {bone.rotation.x * s, bone.rotation.y * s, bone.rotation.z * s, bone.rotation.w * s};
            return;
        }
        float* lanes[] = {&bone.rotation.x, &bone.rotation.y, &bone.rotation.z, &bone.rotation.w,
            &bone.translation[0], &bone.translation[1], &bone.translation[2], &bone.translation[3], &bone.scale[0],
            &bone.scale[1], &bone.scale[2]};
        *lanes[random() % 11u] = values[random() % 12u];
    }

    // A frame made from the previous one (kind 0 to 10): identical (an idle
    // copy), a new pose that agrees, broken objects, broken locals, parents'
    // objects moved alone, opaque lanes only, bones onto a tolerance (on the
    // previous pose or a new one), odd values, a few locals turned with the
    // objects recomposed, scales changed.
    void EditValidationFrame(std::mt19937& random, const SkeletonLayout& layout, bool wide, int kind,
        float translationTolerance, float rotationTolerance, const std::vector<BoneQts>& previousLocals,
        const std::vector<BoneQts>& previousObjects, std::vector<BoneQts>& locals, std::vector<BoneQts>& objects)
    {
        locals = previousLocals;
        objects = previousObjects;
        const std::uint32_t count = layout.boneCount;
        if (count == 0)
            return;
        std::uniform_int_distribution<std::uint32_t> pick(0, count - 1);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        const int edits = 1 + static_cast<int>(random() % 6u);
        switch (kind)
        {
        case 0:
            break;
        case 1:
            RandomValidationPose(random, layout, wide, locals, objects);
            break;
        case 2:
            for (int edit = 0; edit < edits; ++edit)
                objects[pick(random)].translation[random() % 3u] += 0.5f * unit(random);
            break;
        case 3:
            for (int edit = 0; edit < edits; ++edit)
                locals[pick(random)].rotation = AxisAngle(unit(random), unit(random), 1.0f, 2.0f * unit(random));
            break;
        case 4:
            for (int edit = 0; edit < edits; ++edit)
                objects[pick(random)].rotation.w += 0.1f * unit(random);
            break;
        case 5:
            for (int edit = 0; edit < edits; ++edit)
            {
                objects[pick(random)].translation[3] += 1.0f;
                locals[pick(random)].scale[3] += 0.5f;
            }
            break;
        case 6:
        case 7:
            if (kind == 7)
                RandomValidationPose(random, layout, wide, locals, objects);
            for (int edit = 0; edit < edits * 3; ++edit)
                PlaceOnTolerance(random, layout, locals, objects, pick(random), translationTolerance, rotationTolerance);
            break;
        case 8:
            for (int edit = 0; edit < edits; ++edit)
                InjectOddValue(random, random() % 2u ? locals[pick(random)] : objects[pick(random)]);
            break;
        case 9:
            for (int edit = 0; edit < edits; ++edit)
            {
                BoneQts& local = locals[pick(random)];
                local.rotation = Normalize(Multiply(local.rotation, AxisAngle(unit(random), unit(random), 1.0f,
                    0.01f * unit(random))));
                local.translation[random() % 3u] += 0.001f * unit(random);
            }
            ComposeValidationObjects(random, layout, wide, locals, objects);
            break;
        default:
            for (int edit = 0; edit < edits; ++edit)
                objects[pick(random)].scale[random() % 3u] *= 1.0f + 0.2f * unit(random);
            break;
        }
    }

    struct BandTally
    {
        std::uint64_t bones = 0; // parented bones classified
        std::uint64_t fails = 0; // the band decided: fails
        std::uint64_t passes = 0; // the band decided: passes
        std::uint64_t scalar = 0; // left to the scalar test
        std::uint64_t differ = 0; // a band decision against the scalar test
        std::uint64_t naiveDiffer = 0; // the tests on the SSE object itself against the scalar test
    };

    // How the SSE expected object is moved before the verdict
    // (band_nudge::Nudge): not at all, by +-32 of each lane's own ulps, or
    // on the scale of the composition's rounding: +-64 * 2^-24 of the
    // extent per translation lane and +-32 * 2^-24 per rotation lane, about
    // twice and three times what the analysis in pose_skeleton_layout.h
    // allows between the two computations (30 * 2^-24 of the extent and
    // about 10 * 2^-24 per lane).
    struct NudgeMode
    {
        int maxUlps = 0;
        float translationUnits = 0.0f;
        float rotationUnits = 0.0f;
    };
    constexpr NudgeMode kAsComposed{0, 0.0f, 0.0f};
    constexpr NudgeMode kUlpMoves{32, 0.0f, 0.0f};
    constexpr NudgeMode kRoundingMoves{0, 64.0f, 32.0f};

    void InstallNudge(const NudgeMode& mode, std::uint32_t seed)
    {
        band_nudge::Set(mode.maxUlps, mode.translationUnits, mode.rotationUnits, seed);
        SetExpectedObjectHook(&band_nudge::Hook);
    }

    // ValidationVerdict on every parented bone (SSE expected object moved
    // as `mode` says) against the scalar test.
    void TallyBand(const SkeletonLayout& layout, const BoneQts* locals, const BoneQts* objects,
        float translationTolerance, float rotationTolerance, const NudgeMode& mode, std::mt19937& random,
        BandTally& tally)
    {
        const ValidationBand band = MakeValidationBand(translationTolerance, rotationTolerance);
        const AngleTest angle = MakeAngleTest(rotationTolerance);
        for (std::uint32_t bone = 0; bone < layout.boneCount; ++bone)
        {
            const std::int32_t parent = layout.parent[bone];
            if (parent < 0)
                continue;
            const BoneQts& parentObject = objects[static_cast<std::uint32_t>(parent)];
            const BoneQts scalar = Compose(parentObject, locals[bone], layout.order, layout.inheritScale);
            const bool fails = TranslationExceeds(scalar, objects[bone], translationTolerance) ||
                AngleExceeds(angle, scalar.rotation, objects[bone].rotation);
            BoneQts composed;
            ComposeSimd(parentObject, locals[bone], layout.order, layout.inheritScale, composed);
            band_nudge::Nudge(composed, parentObject, mode.maxUlps, mode.translationUnits, mode.rotationUnits, random);
            const bool naive = TranslationExceeds(composed, objects[bone], translationTolerance) ||
                AngleExceeds(angle, composed.rotation, objects[bone].rotation);
            const int verdict = ValidationVerdict(band, parentObject, locals[bone], composed, objects[bone]);
            ++tally.bones;
            tally.naiveDiffer += naive != fails ? 1u : 0u;
            if (verdict == 0)
            {
                ++tally.scalar;
                continue;
            }
            ++(verdict > 0 ? tally.fails : tally.passes);
            tally.differ += (verdict > 0) != fails ? 1u : 0u;
        }
    }

    void PrintBandTally(const char* what, const BandTally& tally)
    {
        std::printf("    %s: %llu parented bones, the band decided %llu (%llu fail, %llu pass), %llu left to the scalar "
                    "test, %llu differ; the plain tests on the SSE object would differ on %llu\n",
            what, static_cast<unsigned long long>(tally.bones),
            static_cast<unsigned long long>(tally.fails + tally.passes), static_cast<unsigned long long>(tally.fails),
            static_cast<unsigned long long>(tally.passes), static_cast<unsigned long long>(tally.scalar),
            static_cast<unsigned long long>(tally.differ), static_cast<unsigned long long>(tally.naiveDiffer));
    }

    // us per call of one implementation on a frame, best of 5 runs of 40
    // calls, the previous arrays restored before every call (not timed).
    double TimeChangedCheck(bool old, const SkeletonLayout& layout, const std::vector<BoneQts>& locals,
        const std::vector<BoneQts>& objects, const std::vector<BoneQts>& previousLocals,
        const std::vector<BoneQts>& previousObjects, std::uint32_t& checked, std::size_t& sink)
    {
        std::vector<BoneQts> workLocals;
        std::vector<BoneQts> workObjects;
        double best = 1e30;
        for (int run = 0; run < 5; ++run)
        {
            double total = 0.0;
            for (int call = 0; call < 40; ++call)
            {
                workLocals = previousLocals;
                workObjects = previousObjects;
                const auto start = std::chrono::steady_clock::now();
                if (old)
                    sink += v175::FindFailingBonesChanged(layout, locals.data(), objects.data(), workLocals.data(),
                        workObjects.data(), 0.002f, 0.004f, checked, nullptr).size();
                else
                    sink += FindFailingBonesChanged(layout, locals.data(), objects.data(), workLocals.data(),
                        workObjects.data(), 0.002f, 0.004f, checked).size();
                total += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            }
            best = std::min(best, total / 40.0);
        }
        return best;
    }

    // One frame through FindFailingBonesChanged as composed and with both
    // kinds of moves (against the reference check), and the verdicts of its
    // parented bones the three ways.
    struct BandRun
    {
        ChangedCheckTally plain;
        ChangedCheckTally ulpMoved;
        ChangedCheckTally roundingMoved;
        BandTally exact;
        BandTally ulpVerdicts;
        BandTally roundingVerdicts;
    };

    void CheckBandFrame(const SkeletonLayout& layout, const std::vector<BoneQts>& locals,
        const std::vector<BoneQts>& objects, const std::vector<BoneQts>& previousLocals,
        const std::vector<BoneQts>& previousObjects, float translationTolerance, float rotationTolerance,
        std::uint32_t seed, bool withCounts, std::mt19937& random, BandRun& run)
    {
        SameChangedCheck(layout, locals.data(), objects.data(), previousLocals.data(), previousObjects.data(),
            translationTolerance, rotationTolerance, run.plain, withCounts);
        InstallNudge(kUlpMoves, seed);
        SameChangedCheck(layout, locals.data(), objects.data(), previousLocals.data(), previousObjects.data(),
            translationTolerance, rotationTolerance, run.ulpMoved, withCounts);
        InstallNudge(kRoundingMoves, seed);
        SameChangedCheck(layout, locals.data(), objects.data(), previousLocals.data(), previousObjects.data(),
            translationTolerance, rotationTolerance, run.roundingMoved, withCounts);
        SetExpectedObjectHook(nullptr);
        TallyBand(layout, locals.data(), objects.data(), translationTolerance, rotationTolerance, kAsComposed, random,
            run.exact);
        TallyBand(layout, locals.data(), objects.data(), translationTolerance, rotationTolerance, kUlpMoves, random,
            run.ulpVerdicts);
        TallyBand(layout, locals.data(), objects.data(), translationTolerance, rotationTolerance, kRoundingMoves,
            random, run.roundingVerdicts);
    }

    void PrintBandRun(const char* what, const BandRun& run)
    {
        std::printf("    %s against reference: %llu frames, %llu bones checked, %llu failing, %llu differ; with the SSE "
                    "object moved +-32 ulps per lane %llu differ, on the rounding scale %llu differ\n",
            what, static_cast<unsigned long long>(run.plain.frames), static_cast<unsigned long long>(run.plain.checked),
            static_cast<unsigned long long>(run.plain.failing), static_cast<unsigned long long>(run.plain.differ),
            static_cast<unsigned long long>(run.ulpMoved.differ),
            static_cast<unsigned long long>(run.roundingMoved.differ));
        PrintBandTally("verdicts, SSE object as composed", run.exact);
        PrintBandTally("verdicts, moved +-32 ulps per lane", run.ulpVerdicts);
        PrintBandTally("verdicts, moved on the rounding scale", run.roundingVerdicts);
    }

    bool BandRunExact(const BandRun& run)
    {
        return run.plain.differ == 0 && run.ulpMoved.differ == 0 && run.roundingMoved.differ == 0 &&
            run.exact.differ == 0 && run.ulpVerdicts.differ == 0 && run.roundingVerdicts.differ == 0;
    }

    void TestValidationBand()
    {
        std::mt19937 random(176);
        BandRun edited;
        BandRun runs;
        std::uint64_t layoutBones = 0;
        std::uint32_t orders[2] = {};
        std::uint32_t inherited[2] = {};
        for (int rig = 0; rig < 360; ++rig)
        {
            std::uint32_t boneCount = 1 + static_cast<std::uint32_t>(random() % 160u);
            if (rig % 45 == 7)
                boneCount = 700;
            const CompositionOrder order =
                rig % 2 ? CompositionOrder::LocalThenParent : CompositionOrder::ParentThenLocal;
            const bool inheritScale = (rig / 2) % 2 == 1;
            const bool wide = rig % 3 != 0; // otherwise engine-like bone lengths
            ++orders[rig % 2];
            ++inherited[inheritScale ? 1 : 0];
            float translationTolerance = 0.002f;
            float rotationTolerance = 0.004f;
            if (rig % 7 == 3)
            {
                translationTolerance = 0.0005f;
                rotationTolerance = 0.01f;
            }
            else if (rig % 11 == 5)
            {
                translationTolerance = 1e-6f; // under the translation margin: no translation pass
            }
            else if (rig % 13 == 9)
            {
                translationTolerance = 0.05f;
                rotationTolerance = 1e-5f; // under the angle margin: the band is off
            }
            const SkeletonLayout layout = RandomValidationLayout(random, boneCount, order, inheritScale);
            layoutBones += boneCount;
            std::vector<BoneQts> previousLocals;
            std::vector<BoneQts> previousObjects;
            RandomValidationPose(random, layout, wide, previousLocals, previousObjects);
            std::vector<BoneQts> locals;
            std::vector<BoneQts> objects;
            for (int kind = 0; kind < 11; ++kind)
            {
                EditValidationFrame(random, layout, wide, kind, translationTolerance, rotationTolerance,
                    previousLocals, previousObjects, locals, objects);
                CheckBandFrame(layout, locals, objects, previousLocals, previousObjects, translationTolerance,
                    rotationTolerance, static_cast<std::uint32_t>(rig * 16 + kind), kind % 2 == 0, random, edited);
            }
            // A run as the worker sees it: each frame the last one with a few
            // locals turned (objects recomposed), now and then an idle copy,
            // a bone onto a tolerance or a broken object.
            std::vector<BoneQts> lastLocals = previousLocals;
            std::vector<BoneQts> lastObjects = previousObjects;
            for (int frame = 0; frame < 24; ++frame)
            {
                const std::uint32_t roll = static_cast<std::uint32_t>(random() % 20u);
                const int kind = roll < 3 ? 0 : roll < 5 ? 6 : roll < 6 ? 2 : 9;
                EditValidationFrame(random, layout, wide, kind, translationTolerance, rotationTolerance, lastLocals,
                    lastObjects, locals, objects);
                CheckBandFrame(layout, locals, objects, lastLocals, lastObjects, translationTolerance,
                    rotationTolerance, static_cast<std::uint32_t>(rig * 64 + frame + 7), true, random, runs);
                lastLocals = locals;
                lastObjects = objects;
            }
        }
        std::printf("  validation band: 360 random layouts (%llu bones, %u/%u parent-then-local/local-then-parent, "
                    "%u/%u without/with inherited scale)\n",
            static_cast<unsigned long long>(layoutBones), orders[0], orders[1], inherited[0], inherited[1]);
        PrintBandRun("edited frames", edited);
        PrintBandRun("frames of worker runs", runs);
        Check(edited.plain.differ == 0 && runs.plain.differ == 0,
            "validation band: FindFailingBonesChanged gives the reference result on random layouts and frames");
        Check(BandRunExact(edited) && BandRunExact(runs),
            "validation band: the same result and verdicts with the SSE expected object moved");
        Check(edited.exact.naiveDiffer == 0 && runs.exact.naiveDiffer == 0,
            "validation band: ComposeSimd is the scalar Compose here (/fp:precise)");
        Check(edited.ulpVerdicts.naiveDiffer > 0 && edited.roundingVerdicts.naiveDiffer > 0,
            "validation band: the moves flip decisions of the plain tests");
        Check(edited.exact.fails > 0 && edited.exact.passes > 0 && edited.exact.scalar > 0,
            "validation band: frames fail, pass and fall back to the scalar test");

        // Bones right on the tolerances (a few ulps either side) and around
        // them (+-5%, across the band's edges) at every magnitude: per case
        // a root and its child (both orders, inherited scale on and off,
        // engine-like and wide lengths), the child's object placed by
        // PlaceOnTolerance; each case through CheckBandFrame, then 6 more
        // verdicts with each kind of move.
        BandRun onTolerance;
        BandRun aroundTolerance;
        for (int index = 0; index < 40000; ++index)
        {
            const bool around = index >= 20000;
            SkeletonLayout layout;
            layout.boneCount = 2;
            layout.order = index % 2 ? CompositionOrder::LocalThenParent : CompositionOrder::ParentThenLocal;
            layout.inheritScale = (index / 2) % 2 == 1;
            layout.parent = {kRootParent, 0};
            layout.ambiguous.assign(2, 0);
            layout.highPrecision.assign(2, 0);
            DemoteBones(layout, {});
            const bool wide = (index / 4) % 2 == 1;
            std::vector<BoneQts> locals;
            std::vector<BoneQts> objects;
            RandomValidationPose(random, layout, wide, locals, objects);
            std::vector<BoneQts> previousLocals = locals;
            std::vector<BoneQts> previousObjects = objects;
            previousObjects[1].translation[3] += 1.0f; // the child is checked
            PlaceOnTolerance(random, layout, locals, objects, 1, 0.002f, 0.004f, around ? 0.05f : 0.0f);
            BandRun& run = around ? aroundTolerance : onTolerance;
            CheckBandFrame(layout, locals, objects, previousLocals, previousObjects, 0.002f, 0.004f,
                static_cast<std::uint32_t>(index), true, random, run);
            for (int move = 0; move < 6; ++move)
            {
                TallyBand(layout, locals.data(), objects.data(), 0.002f, 0.004f, kUlpMoves, random, run.ulpVerdicts);
                TallyBand(layout, locals.data(), objects.data(), 0.002f, 0.004f, kRoundingMoves, random,
                    run.roundingVerdicts);
            }
        }
        std::printf("    on and around the tolerances: 20000 bones within +-12 ulps of 2 mm or +-768 ulps of 4 mrad, "
                    "20000 within +-5%%, 7 moved verdicts of each kind per bone\n");
        PrintBandRun("on the tolerances", onTolerance);
        PrintBandRun("around the tolerances", aroundTolerance);
        Check(BandRunExact(onTolerance) && BandRunExact(aroundTolerance),
            "validation band: bones on and around the tolerances keep the scalar decision under the moves");
        Check(onTolerance.ulpVerdicts.naiveDiffer > 0 && onTolerance.roundingVerdicts.naiveDiffer > 0 &&
                onTolerance.exact.scalar > 0 && aroundTolerance.exact.fails > 0 &&
                aroundTolerance.exact.passes > 0 && aroundTolerance.exact.scalar > 0,
            "validation band: the tolerance cases reach the scalar test, the band decides either way around them");

        // Cost on an engine-like 700-bone skeleton (parent-then-local with
        // inherited scale, parents before children, bones of 2 to 40 cm):
        // every bone moved, half of the leaf bones turned (nothing else
        // moves), an identical frame.
        {
            SkeletonLayout layout;
            layout.boneCount = 700;
            layout.order = CompositionOrder::ParentThenLocal;
            layout.inheritScale = true;
            layout.parent.assign(700, kRootParent);
            layout.ambiguous.assign(700, 0);
            layout.highPrecision.assign(700, 0);
            for (std::uint32_t bone = 1; bone < 700; ++bone)
                layout.parent[bone] = static_cast<std::int32_t>(random() % 10u < 6u ? bone - 1 : random() % bone);
            DemoteBones(layout, {});
            std::vector<BoneQts> previousLocals;
            std::vector<BoneQts> previousObjects;
            RandomValidationPose(random, layout, false, previousLocals, previousObjects);
            for (BoneQts& bone : previousLocals)
            {
                for (int axis = 0; axis < 4; ++axis)
                    bone.scale[axis] = 1.0f;
            }
            ComposeValidationObjects(random, layout, false, previousLocals, previousObjects);
            std::vector<std::uint8_t> hasChild(700, 0);
            for (std::uint32_t bone = 1; bone < 700; ++bone)
                hasChild[static_cast<std::uint32_t>(layout.parent[bone])] = 1;
            std::vector<BoneQts> movedLocals = previousLocals;
            std::vector<BoneQts> leafLocals = previousLocals;
            std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
            for (std::uint32_t bone = 0; bone < 700; ++bone)
            {
                const Quat turn = AxisAngle(unit(random), unit(random), 1.0f, 0.02f);
                movedLocals[bone].rotation = Normalize(Multiply(movedLocals[bone].rotation, turn));
                if (!hasChild[bone] && bone % 2 == 0)
                    leafLocals[bone].rotation = movedLocals[bone].rotation;
            }
            std::vector<BoneQts> movedObjects = previousObjects;
            std::vector<BoneQts> leafObjects = previousObjects;
            ComposeValidationObjects(random, layout, false, movedLocals, movedObjects);
            ComposeValidationObjects(random, layout, false, leafLocals, leafObjects);
            std::size_t sink = 0;
            std::uint32_t movedChecked = 0, leafChecked = 0, sameChecked = 0, unused = 0;
            const double movedOld = TimeChangedCheck(true, layout, movedLocals, movedObjects, previousLocals,
                previousObjects, unused, sink);
            const double movedNew = TimeChangedCheck(false, layout, movedLocals, movedObjects, previousLocals,
                previousObjects, movedChecked, sink);
            const double leafOld = TimeChangedCheck(true, layout, leafLocals, leafObjects, previousLocals,
                previousObjects, unused, sink);
            const double leafNew = TimeChangedCheck(false, layout, leafLocals, leafObjects, previousLocals,
                previousObjects, leafChecked, sink);
            const double sameOld = TimeChangedCheck(true, layout, previousLocals, previousObjects, previousLocals,
                previousObjects, unused, sink);
            const double sameNew = TimeChangedCheck(false, layout, previousLocals, previousObjects, previousLocals,
                previousObjects, sameChecked, sink);
            std::printf("  validation of a 700-bone engine-like frame, us per call reference -> current: every bone "
                        "moved %.2f -> %.2f (%u recomputed), leaves turned %.2f -> %.2f (%u recomputed), identical "
                        "%.2f -> %.2f (%u recomputed); sink %zu\n",
                movedOld, movedNew, movedChecked, leafOld, leafNew, leafChecked, sameOld, sameNew, sameChecked, sink);
            Check(sameChecked == 0 && movedChecked == 700 && leafChecked > 0 && leafChecked < 700,
                "validation band: the benchmark frames recompute every bone, the turned leaves and nothing");
        }
    }

    // A bone whose parent stopped matching gets its true parent
    // back from the frames kept, instead of staying in object space.
    void TestReparentAfterContradiction()
    {
        const SyntheticSkeleton skeleton;
        std::vector<BoneQts> locals[5];
        std::vector<BoneQts> objects[5];
        DiscoveryFrame frames[5];
        for (int frame = 0; frame < 5; ++frame)
        {
            skeleton.Pose(0.4f * frame + 0.1f, CompositionOrder::ParentThenLocal, locals[frame], objects[frame]);
            frames[frame] = {locals[frame].data(), objects[frame].data()};
        }
        SkeletonLayout layout;
        DiscoveryStats stats;
        Check(DiscoverSkeletonLayout(frames, 3, skeleton.Count(), {}, layout, stats), "layout discovered");

        // A wrong parent (as an ambiguous pick would give) fails on a later
        // frame, is demoted, then re-parented to the real one.
        const std::uint32_t finger = skeleton.Count() - 8;
        const std::int32_t truth = skeleton.parent[finger];
        layout.parent[finger] = 0;
        DemoteBones(layout, {});
        const std::vector<std::uint32_t> failing =
            FindFailingBones(layout, locals[3].data(), objects[3].data(), 0.002f, 0.004f);
        Check(std::find(failing.begin(), failing.end(), finger) != failing.end(),
            "the wrong parent fails on a new frame");
        DemoteBones(layout, {finger});
        Check(layout.parent[finger] == kUnresolvedParent && layout.unresolvedBones.size() == 1,
            "the bone is demoted first");
        Check(ReparentBones(layout, {finger}, frames, 4, {}) == 1 && layout.parent[finger] == truth &&
                layout.unresolvedBones.empty(),
            "re-parenting finds the real parent from the kept frames");

        // Nothing explains a bone whose object transform is corrupt: it
        // stays in object space.
        std::vector<BoneQts> broken = objects[3];
        broken[finger].translation[2] += 0.5f;
        DiscoveryFrame withBroken[4] = {frames[0], frames[1], frames[2], {locals[3].data(), broken.data()}};
        DemoteBones(layout, {finger});
        Check(ReparentBones(layout, {finger}, withBroken, 4, {}) == 0 && layout.parent[finger] == kUnresolvedParent,
            "a bone no parent explains stays in object space");
        // A bone already parented is left alone.
        Check(ReparentBones(layout, {1}, frames, 4, {}) == 0 && layout.parent[1] == skeleton.parent[1],
            "re-parenting only touches object-space bones");
    }

    // The periodic keyframes are encoded against the anchor (the
    // first full keyframe of the layout) and cost a fraction of it; a chunk
    // decoded from scratch still reproduces the sequential decode.
    void TestRelativeKeyframes()
    {
        const SyntheticSkeleton skeleton;
        std::vector<BoneQts> locals[3];
        std::vector<BoneQts> objects[3];
        DiscoveryFrame frames[3];
        for (int frame = 0; frame < 3; ++frame)
        {
            skeleton.Pose(0.3f * frame + 0.1f, CompositionOrder::ParentThenLocal, locals[frame], objects[frame]);
            frames[frame] = {locals[frame].data(), objects[frame].data()};
        }
        SkeletonLayout layout;
        DiscoveryStats stats;
        Check(DiscoverSkeletonLayout(frames, 3, skeleton.Count(), {}, layout, stats), "layout for relative keyframes");

        PoseTrack track;
        StreamConfig config;
        config.keyframeIntervalMs = 1000;
        track.Begin(layout, config);
        const float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
        constexpr std::uint32_t kFrames = 150; // 5 s at 30 Hz: 1 full + 4 relative keyframes
        std::vector<BoneQts> l, o;
        for (std::uint32_t frame = 0; frame < kFrames; ++frame)
        {
            skeleton.Pose(0.02f * frame, CompositionOrder::ParentThenLocal, l, o);
            FrameInput input;
            input.timeMs = frame * 33u;
            input.world = world;
            input.locals = l.data();
            input.objects = o.data();
            track.Append(input);
        }
        const TrackStats& ts = track.Stats();
        Check(ts.keyframes == 5 && ts.relativeKeyframes == 4, "one full keyframe, four relative ones");
        Check(ts.relativeKeyframeBytes * 2u < ts.fullKeyframeBytes * 4u,
            "a relative keyframe costs under half of a full one (resting bones carry nothing)");

        // A fresh track sampled straight into the fourth chunk equals the
        // one that walked there frame by frame.
        PoseTrack fresh;
        fresh.Begin(layout, config);
        for (std::uint32_t frame = 0; frame < kFrames; ++frame)
        {
            skeleton.Pose(0.02f * frame, CompositionOrder::ParentThenLocal, l, o);
            FrameInput input;
            input.timeMs = frame * 33u;
            input.world = world;
            input.locals = l.data();
            input.objects = o.data();
            fresh.Append(input);
        }
        PoseSample walked;
        bool ok = true;
        for (std::uint32_t t = 0; t <= 4000 && ok; t += 16)
            ok = track.Sample(t, walked);
        PoseSample jumped;
        ok = ok && fresh.Sample(4000, jumped) && track.Sample(4000, walked);
        Check(ok && walked.locals.size() == jumped.locals.size() &&
                std::memcmp(walked.locals.data(), jumped.locals.data(), walked.locals.size() * sizeof(BoneQts)) == 0 &&
                std::memcmp(walked.objects.data(), jumped.objects.data(), walked.objects.size() * sizeof(BoneQts)) == 0,
            "a seek into a relative-keyframe chunk equals the sequential decode");

        // A layout change starts a new anchor (full keyframe).
        SkeletonLayout demoted = layout;
        DemoteBones(demoted, {skeleton.Count() - 8});
        track.UpdateLayout(demoted);
        skeleton.Pose(0.02f * kFrames, CompositionOrder::ParentThenLocal, l, o);
        FrameInput input;
        input.timeMs = kFrames * 33u;
        input.world = world;
        input.locals = l.data();
        input.objects = o.data();
        track.Append(input);
        Check(track.Stats().keyframes == 6 && track.Stats().relativeKeyframes == 4,
            "a layout change writes a full keyframe (new anchor)");
    }

    // The layout built from the engine's parent table explains
    // the frames and matches the tree; a scaled root goes to object space.
    void TestLayoutFromEngineParents()
    {
        const SyntheticSkeleton skeleton;
        std::vector<BoneQts> locals[3];
        std::vector<BoneQts> objects[3];
        DiscoveryFrame frames[3];
        for (int frame = 0; frame < 3; ++frame)
        {
            skeleton.Pose(0.3f * frame + 0.1f, CompositionOrder::ParentThenLocal, locals[frame], objects[frame], true);
            frames[frame] = {locals[frame].data(), objects[frame].data()};
        }
        std::vector<std::uint16_t> parents(skeleton.Count(), 0xFFFFu);
        for (std::uint32_t bone = 1; bone < skeleton.Count(); ++bone)
            parents[bone] = static_cast<std::uint16_t>(skeleton.parent[bone]);
        SkeletonLayout layout;
        DiscoveryStats stats;
        Check(BuildLayoutFromParents(parents.data(), skeleton.Count(), false, {}, layout, stats),
            "layout built from the engine parents");
        bool same = layout.parent[0] == kRootParent && layout.inheritScale && layout.order == CompositionOrder::ParentThenLocal;
        for (std::uint32_t bone = 1; bone < skeleton.Count() && same; ++bone)
            same = layout.parent[bone] == skeleton.parent[bone];
        Check(same && stats.unresolved == 0 && stats.ambiguous == 0, "engine parents copied one to one, nothing ambiguous");
        Check(LayoutExplainsFrames(layout, frames, 3, skeleton.Count(), {}),
            "the engine layout explains scaled-rig frames");
        SkeletonLayout scaledRoot;
        Check(BuildLayoutFromParents(parents.data(), skeleton.Count(), true, {}, scaledRoot, stats) &&
                scaledRoot.parent[0] == kUnresolvedParent && scaledRoot.unresolvedBones.size() == 1,
            "a scaled root is recorded in object space");
        std::vector<std::uint16_t> bad = parents;
        bad[5] = 9; // a parent that comes later
        Check(BuildLayoutFromParents(bad.data(), skeleton.Count(), false, {}, layout, stats) &&
                layout.parent[5] == kUnresolvedParent,
            "a parent that is not an earlier bone goes to object space");
    }

    // End-of-frame pose against the finalizer's output.
    void TestCompareLatePose()
    {
        const SyntheticSkeleton skeleton;
        std::vector<BoneQts> locals;
        std::vector<BoneQts> objects;
        skeleton.Pose(0.7f, CompositionOrder::ParentThenLocal, locals, objects, true);
        std::vector<std::uint16_t> parents(skeleton.Count(), 0xFFFFu);
        for (std::uint32_t bone = 1; bone < skeleton.Count(); ++bone)
            parents[bone] = static_cast<std::uint16_t>(skeleton.parent[bone]);
        std::vector<BoneQts> scratch;
        LatePoseChange same = CompareLatePose(locals.data(), locals.data(), objects.data(), parents.data(),
            skeleton.Count(), 1.0f, 0.01f, 0.01f, scratch);
        Check(same.localBones == 0 && same.objectBones == 0 && same.worstBone == -1,
            "an untouched pose shows no late change");

        // A helper bone (a root child, last of the props) moved 4 m by a
        // late local write, objects recomposed: one local change only.
        const std::uint32_t helper = skeleton.Count() - 1;
        std::vector<BoneQts> lateLocals = locals;
        lateLocals[helper].translation[1] += 4.0f;
        std::vector<BoneQts> lateObjects(skeleton.Count());
        for (std::uint32_t bone = 0; bone < skeleton.Count(); ++bone)
        {
            lateObjects[bone] = skeleton.parent[bone] < 0 ? lateLocals[bone]
                : Compose(lateObjects[static_cast<std::uint32_t>(skeleton.parent[bone])], lateLocals[bone],
                      CompositionOrder::ParentThenLocal, true);
        }
        LatePoseChange local = CompareLatePose(locals.data(), lateLocals.data(), lateObjects.data(), parents.data(),
            skeleton.Count(), 1.0f, 0.01f, 0.01f, scratch);
        Check(local.localBones == 1 && local.objectBones == 0 && local.worstBone == static_cast<std::int32_t>(helper) &&
                local.maxTranslation > 3.9f,
            "a late local write is found on its bone");

        // A hand object written directly (ragdoll or IK style): only that
        // bone, not its fingers, whose objects follow the written parent.
        const std::uint32_t hand = 8;
        std::vector<BoneQts> written = objects;
        written[hand].translation[2] += 0.3f;
        for (std::uint32_t bone = hand + 1; bone < skeleton.Count(); ++bone)
        {
            if (skeleton.parent[bone] >= static_cast<std::int32_t>(hand))
                written[bone] = Compose(written[static_cast<std::uint32_t>(skeleton.parent[bone])], locals[bone],
                    CompositionOrder::ParentThenLocal, true);
        }
        LatePoseChange object = CompareLatePose(locals.data(), locals.data(), written.data(), parents.data(),
            skeleton.Count(), 1.0f, 0.01f, 0.01f, scratch);
        Check(object.localBones == 0 && object.objectBones == 1 && object.worstBone == static_cast<std::int32_t>(hand),
            "a late object write blames its own bone only");
    }

    // The playback check compares the written pose as it
    // was written, locals and objects, and bounds the changed bones.
    void TestCompareWrittenPose()
    {
        const SyntheticSkeleton skeleton;
        std::vector<BoneQts> locals;
        std::vector<BoneQts> objects;
        skeleton.Pose(0.4f, CompositionOrder::ParentThenLocal, locals, objects, true);
        const std::uint32_t count = skeleton.Count();
        std::vector<BoneQts> written(locals);
        written.insert(written.end(), objects.begin(), objects.end());
        WrittenPoseChange same = CompareWrittenPose(written.data(), written.data(), count, 0.01f, 0.01f);
        Check(same.localBones == 0 && same.objectBones == 0 && same.firstBone == -1 && same.worstBone == -1,
            "an untouched written pose shows no change");

        // A later pass rewrites the objects of the last two bones (a layer's
        // group at the end of the skeleton): both found, bounded, locals clean.
        std::vector<BoneQts> now = written;
        now[count + count - 2].translation[0] += 0.5f;
        now[count + count - 1].translation[2] -= 1.5f;
        WrittenPoseChange group = CompareWrittenPose(written.data(), now.data(), count, 0.01f, 0.01f);
        Check(group.localBones == 0 && group.objectBones == 2 &&
                group.firstBone == static_cast<std::int32_t>(count - 2) &&
                group.lastBone == static_cast<std::int32_t>(count - 1) &&
                group.worstBone == static_cast<std::int32_t>(count - 1) && group.maxTranslation > 1.4f,
            "object rewrites after the override are bounded to their bones");

        // A local rotated past the tolerance, under it elsewhere.
        now = written;
        now[1].rotation = Normalize(Multiply(now[1].rotation, Quat{0.0f, 0.0f, 0.05f, 1.0f}));
        now[2].translation[1] += 0.005f;
        WrittenPoseChange local = CompareWrittenPose(written.data(), now.data(), count, 0.01f, 0.01f);
        Check(local.localBones == 1 && local.objectBones == 0 && local.firstBone == 1 && local.lastBone == 1 &&
                local.maxRotation > 0.05f,
            "a rotated local counts, a translation under the tolerance does not");
    }

    void TestStaticBonesCostNothing()
    {
        Recorded recorded = MakeRecording(12);
        StreamConfig config;
        PoseEncoder encoder;
        encoder.Begin(recorded.layout, config);
        std::vector<std::uint8_t> bytes;
        FrameInput input;
        input.locals = recorded.locals[0].data();
        input.objects = recorded.objects[0].data();
        const EncodeStats keyframe = encoder.Encode(input, KeyframeKind::Full, bytes);
        const EncodeStats repeat = encoder.Encode(input, KeyframeKind::None, bytes);
        Check(keyframe.entries == recorded.layout.ChannelCount(),
            "keyframe carries every channel");
        Check(repeat.entries == 0, "an unchanged pose emits no bone entries");
    }

    void TestDecoderRejectsBadInput()
    {
        Recorded recorded = MakeRecording(12);
        StreamConfig config;
        PoseEncoder encoder;
        encoder.Begin(recorded.layout, config);
        std::vector<std::uint8_t> keyframe;
        std::vector<std::uint8_t> delta;
        FrameInput input;
        input.locals = recorded.locals[0].data();
        input.objects = recorded.objects[0].data();
        encoder.Encode(input, KeyframeKind::Full, keyframe);
        input.locals = recorded.locals[4].data();
        input.objects = recorded.objects[4].data();
        encoder.Encode(input, KeyframeKind::None, delta);

        PoseDecoder decoder;
        decoder.Begin(recorded.layout, config);
        std::size_t consumed = 0;
        Check(!decoder.Apply(delta.data(), delta.size(), consumed),
            "a delta frame before any keyframe is rejected");
        Check(!decoder.Apply(keyframe.data(), keyframe.size() - 1, consumed),
            "a truncated record is rejected");
        Check(decoder.Apply(keyframe.data(), keyframe.size(), consumed) &&
                consumed == keyframe.size(),
            "a complete keyframe decodes");
        const std::vector<BoneQts> before = decoder.Channels();
        std::vector<std::uint8_t> corrupt = delta;
        const std::uint16_t badChannel = 0xFFFF;
        std::memcpy(corrupt.data() + 76, &badChannel, sizeof(badChannel));
        Check(!decoder.Apply(corrupt.data(), corrupt.size(), consumed),
            "an out-of-range channel is rejected");
        Check(std::memcmp(before.data(), decoder.Channels().data(),
                  before.size() * sizeof(BoneQts)) == 0,
            "a rejected record leaves decoder state untouched");
    }

    // An export shares the closed chunks with the track,
    // and the track writing afterwards never changes what was exported.
    void TestExportSharesChunks()
    {
        Recorded recorded = MakeRecording(90);
        StreamConfig config;
        config.keyframeIntervalMs = 330;
        PoseTrack track;
        track.Begin(recorded.layout, config);
        auto append = [&](std::uint32_t from, std::uint32_t to) {
            for (std::uint32_t frame = from; frame < to; ++frame)
            {
                FrameInput input;
                input.timeMs = frame * 33u;
                input.locals = recorded.locals[frame].data();
                input.objects = recorded.objects[frame].data();
                track.Append(input);
            }
        };
        append(0, 45);
        TrackExport first;
        track.Export(first);
        std::vector<std::vector<std::uint8_t>> copies;
        for (const TrackChunkExport& chunk : first.chunks)
            copies.push_back(chunk.bytes.Get());
        append(45, 90);
        TrackExport second;
        track.Export(second);
        Check(first.chunks.size() >= 2 && second.chunks.size() > first.chunks.size(), "export sharing: several chunks");
        Check(first.chunks.front().bytes.SharesWith(second.chunks.front().bytes),
            "export sharing: a closed chunk is shared, not copied");
        bool unchanged = true;
        for (std::size_t index = 0; index < first.chunks.size(); ++index)
            unchanged = unchanged && first.chunks[index].bytes == copies[index];
        Check(unchanged, "export sharing: writing after an export leaves it as it was");
        Check(second.chunks[first.chunks.size() - 1].bytes.size() >= copies.back().size(),
            "export sharing: the open chunk kept growing in the track");
        // A window cut writes its own copy (Mutable), never the track's.
        TrackChunkExport cut = second.chunks.front();
        cut.bytes.Mutable()[0] ^= 0xFF;
        Check(!cut.bytes.SharesWith(second.chunks.front().bytes) && second.chunks.front().bytes == copies.front(),
            "export sharing: a write to a shared copy unshares it first");
    }

    // Helpers of TestSealedTrack. Two samples bit for bit; the recorded data of
    // two exports chunk by chunk (layouts and anchors by content); two exports
    // byte for byte (the same indices, tables, config and stats too).
    bool SameSample(const PoseSample& a, const PoseSample& b)
    {
        return a.t == b.t && a.frameTimeA == b.frameTimeA && a.frameTimeB == b.frameTimeB &&
            a.locals.size() == b.locals.size() && a.objects.size() == b.objects.size() &&
            std::memcmp(a.locals.data(), b.locals.data(), a.locals.size() * sizeof(BoneQts)) == 0 &&
            std::memcmp(a.objects.data(), b.objects.data(), a.objects.size() * sizeof(BoneQts)) == 0 &&
            std::memcmp(a.worldA, b.worldA, sizeof(a.worldA)) == 0 &&
            std::memcmp(a.worldB, b.worldB, sizeof(a.worldB)) == 0;
    }

    bool SameLayoutFields(const SkeletonLayout& a, const SkeletonLayout& b)
    {
        return a.boneCount == b.boneCount && a.order == b.order && a.inheritScale == b.inheritScale &&
            a.parent == b.parent && a.ambiguous == b.ambiguous && a.highPrecision == b.highPrecision &&
            a.evaluationOrder == b.evaluationOrder && a.unresolvedBones == b.unresolvedBones;
    }

    bool SameStats(const TrackStats& a, const TrackStats& b)
    {
        return a.frames == b.frames && a.keyframes == b.keyframes && a.relativeKeyframes == b.relativeKeyframes &&
            a.fullKeyframeBytes == b.fullKeyframeBytes && a.relativeKeyframeBytes == b.relativeKeyframeBytes &&
            a.bytes == b.bytes && a.droppedChunks == b.droppedChunks && a.firstTimeMs == b.firstTimeMs &&
            a.lastTimeMs == b.lastTimeMs;
    }

    bool SameChunks(const TrackExport& a, const TrackExport& b)
    {
        if (a.chunks.size() != b.chunks.size())
            return false;
        for (std::size_t index = 0; index < a.chunks.size(); ++index)
        {
            const TrackChunkExport& ca = a.chunks[index];
            const TrackChunkExport& cb = b.chunks[index];
            if (ca.bytes != cb.bytes || ca.frameTimes != cb.frameTimes || ca.frameOffsets != cb.frameOffsets ||
                ca.layout >= a.layouts.size() || cb.layout >= b.layouts.size() ||
                !SameLayoutFields(a.layouts[ca.layout], b.layouts[cb.layout]) || (ca.anchor < 0) != (cb.anchor < 0))
            {
                return false;
            }
            if (ca.anchor >= 0 &&
                (static_cast<std::size_t>(ca.anchor) >= a.anchors.size() ||
                    static_cast<std::size_t>(cb.anchor) >= b.anchors.size() ||
                    a.anchors[static_cast<std::size_t>(ca.anchor)] != b.anchors[static_cast<std::size_t>(cb.anchor)]))
            {
                return false;
            }
        }
        return true;
    }

    bool SameExport(const TrackExport& a, const TrackExport& b)
    {
        if (!SameChunks(a, b) || a.layouts.size() != b.layouts.size() || a.anchors != b.anchors ||
            a.currentLayout != b.currentLayout || !SameStats(a.stats, b.stats) ||
            a.config.rotationEpsilon != b.config.rotationEpsilon ||
            a.config.translationEpsilon != b.config.translationEpsilon ||
            a.config.translationRange != b.config.translationRange ||
            a.config.keyframeIntervalMs != b.config.keyframeIntervalMs || a.config.byteBudget != b.config.byteBudget)
        {
            return false;
        }
        for (std::size_t index = 0; index < a.layouts.size(); ++index)
        {
            if (!SameLayoutFields(a.layouts[index], b.layouts[index]))
                return false;
        }
        for (std::size_t index = 0; index < a.chunks.size(); ++index)
        {
            if (a.chunks[index].layout != b.chunks[index].layout || a.chunks[index].anchor != b.chunks[index].anchor)
                return false;
        }
        return true;
    }

    // The editor's playhead moves: play at 60 fps, slow motion, 2x and
    // fine backward scrub, seeks followed by play and by a scrub back, and
    // times before and after the track.
    std::vector<std::uint32_t> SealPath(std::uint32_t first, std::uint32_t last)
    {
        std::vector<std::uint32_t> times;
        const auto forward = [&](std::uint32_t from, std::uint32_t to, std::uint32_t step) {
            for (std::uint32_t time = from; time <= to; time += step)
                times.push_back(time);
        };
        const auto backward = [&](std::uint32_t from, std::uint32_t to, std::uint32_t step) {
            for (std::uint32_t time = from; time >= to + step; time -= step)
                times.push_back(time);
        };
        for (std::uint32_t time = first, tick = 0; time <= last; time += (tick++ % 3 == 0) ? 17u : 16u)
            times.push_back(time);                                          // 60 fps play
        forward(first + 1000u, std::min(last, first + 1900u), 4u);          // 0.25x slow motion
        backward(last, first, 33u);                                         // 2x scrub back
        backward(std::min(last, first + 4300u), first + 3000u, 10u);        // fine scrub back
        std::mt19937 random(176);
        const std::uint32_t span = last - first;
        for (int seek = 0; seek < 24; ++seek)
        {
            const std::uint32_t at = first + static_cast<std::uint32_t>(random() % (span + 1u));
            forward(at, std::min(last, at + 300u), 16u);                     // seek, then play
            backward(std::min(last, at + 300u), at > 200u ? at - 200u : 0u, 23u); // and back
        }
        times.push_back(0u);                                                // before the first frame
        times.push_back(last + 5000u);                                      // after the last
        return times;
    }

    // Seal frees the encoder state and the playback caches of a track that is
    // done recording. The sealed track exports the same bytes and samples the
    // same poses as the unsealed one at every playhead step (sealed once before
    // playing, and sealed again every few samples while playing), DropBefore
    // drops the same chunks, an Append after Seal opens a new chunk with a full
    // keyframe that decodes (the bytes UpdateLayout with the same layout
    // writes), and Begin, Import and Seal leave the decoders without channels.
    void TestSealedTrack()
    {
        constexpr std::uint32_t kRecorded = 230; // 7.6 s at 30 fps: 2 s chunks, relative keyframes, a layout change
        constexpr std::uint32_t kAppended = 70;  // appended after Seal
        constexpr std::size_t kSealEvery = 7;
        Recorded recorded = MakeRecording(kRecorded + kAppended);
        SkeletonLayout demoted = recorded.layout;
        DemoteBones(demoted, {recorded.skeleton.Count() - 8});
        const StreamConfig config{};
        float world[16] = {};
        world[0] = 1.0f;
        world[5] = 1.0f;
        world[10] = 1.0f;
        const auto frameTime = [](std::uint32_t frame) { return frame * 33u + (frame % 4 == 1 ? 1u : 0u); };
        const auto append = [&](PoseTrack& target, std::uint32_t frame) {
            world[12] = 0.04f * static_cast<float>(frame) + (frame >= 97 ? 6.0f : 0.0f); // a teleport no flag marks
            world[13] = 1.0f;
            FrameInput input;
            input.timeMs = frameTime(frame);
            input.flags = (frame % 41 == 17) ? kFrameNoInterpolation : 0;
            input.world = world;
            input.locals = recorded.locals[frame].data();
            input.objects = recorded.objects[frame].data();
            return target.Append(input);
        };

        PoseTrack track;
        track.Begin(recorded.layout, config);
        Check(!track.Sealed() && track.EncoderBytes() > 0 && track.PlaybackCacheBytes() == 0,
            "sealed track: Begin Begins the encoder and leaves the decoders empty");
        for (std::uint32_t frame = 0; frame < kRecorded; ++frame)
        {
            if (frame == 150)
                track.UpdateLayout(demoted);
            append(track, frame);
        }
        TrackExport before;
        track.Export(before);
        Check(before.layouts.size() == 2 && before.anchors.size() == 2 && track.Stats().relativeKeyframes >= 2,
            "sealed track: the track has two layouts, two anchors and relative keyframes");
        const PoseTrack pristine = track; // never sampled
        const std::size_t encoderBytes = track.EncoderBytes();
        const std::uint32_t channels = track.Layout().ChannelCount();

        // Sampling: the unsealed track, one sealed before playing and one
        // sealed again every kSealEvery samples, over the same path.
        const std::vector<std::uint32_t> path = SealPath(track.Stats().firstTimeMs, track.Stats().lastTimeMs);
        PoseTrack plain = pristine;
        PoseTrack sealedOnce = pristine;
        PoseTrack resealed = pristine;
        sealedOnce.Seal();
        std::size_t sampled = 0;
        std::size_t differOnce = 0;
        std::size_t differResealed = 0;
        std::size_t seals = 0;
        PoseSample a;
        PoseSample b;
        PoseSample c;
        for (std::size_t index = 0; index < path.size(); ++index)
        {
            if (index % kSealEvery == 3)
            {
                resealed.Seal();
                ++seals;
            }
            const bool okA = plain.Sample(path[index], a);
            const bool okB = sealedOnce.Sample(path[index], b);
            const bool okC = resealed.Sample(path[index], c);
            sampled += okA ? 1u : 0u;
            differOnce += (okA != okB || (okA && !SameSample(a, b))) ? 1u : 0u;
            differResealed += (okA != okC || (okA && !SameSample(a, c))) ? 1u : 0u;
        }
        Check(sampled == path.size() && differOnce == 0 && differResealed == 0,
            "sealed track: Sample equals the unsealed track's at every step (sealed once, and sealed every 7 samples while playing)");
        Check(plain.UnflaggedRebaseHolds() > 0 && plain.UnflaggedRebaseHolds() == sealedOnce.UnflaggedRebaseHolds() &&
                plain.UnflaggedRebaseHolds() == resealed.UnflaggedRebaseHolds() && plain.CheckpointRestores() > 0,
            "sealed track: the same unflagged rebase holds, and the path goes through the scrub checkpoints");

        // Memory: what the unsealed track keeps, what Seal frees.
        const std::size_t cacheBytes = plain.PlaybackCacheBytes();
        Check(encoderBytes > 0 && plain.EncoderBytes() > 0 && cacheBytes > 0 && plain.CheckpointBytes() > 0,
            "sealed track: the unsealed track keeps its encoder state and its playback caches");
        Check(sealedOnce.Sealed() && resealed.Sealed() && sealedOnce.EncoderBytes() == 0 &&
                sealedOnce.PlaybackCacheBytes() > 0,
            "sealed track: Sample rebuilds the playback caches of a sealed track, not its encoder, and keeps it sealed");
        plain.Seal();
        sealedOnce.Seal();
        resealed.Seal();
        track.Seal();
        Check(plain.Sealed() && plain.EncoderBytes() == 0 && plain.PlaybackCacheBytes() == 0 &&
                plain.CheckpointBytes() == 0 && sealedOnce.PlaybackCacheBytes() == 0 &&
                resealed.PlaybackCacheBytes() == 0 && track.EncoderBytes() == 0 && track.PlaybackCacheBytes() == 0,
            "sealed track: the sealed encoder and decoders hold no memory");

        // Export, stats and layout after Seal.
        TrackExport afterPlain;
        TrackExport afterOnce;
        TrackExport afterResealed;
        TrackExport afterTrack;
        plain.Export(afterPlain);
        sealedOnce.Export(afterOnce);
        resealed.Export(afterResealed);
        track.Export(afterTrack);
        Check(SameExport(before, afterPlain) && SameExport(before, afterOnce) && SameExport(before, afterResealed) &&
                SameExport(before, afterTrack),
            "sealed track: Export after Seal is byte-identical to Export before");
        Check(SameStats(track.Stats(), pristine.Stats()) && track.Bytes() == pristine.Bytes() &&
                SameLayoutFields(track.Layout(), pristine.Layout()),
            "sealed track: stats, bytes and layout stay");

        // DropBefore: the same chunks dropped from a played track, sealed or
        // not, and the rest samples the same.
        {
            PoseTrack keptCaches = pristine;
            PoseTrack sealedCaches = pristine;
            for (std::size_t index = 0; index < path.size(); index += 5)
            {
                keptCaches.Sample(path[index], a);
                sealedCaches.Sample(path[index], b);
            }
            sealedCaches.Seal();
            const std::size_t droppedKept = keptCaches.DropBefore(4500u);
            const std::size_t droppedSealed = sealedCaches.DropBefore(4500u);
            TrackExport keptExport;
            TrackExport sealedExport;
            keptCaches.Export(keptExport);
            sealedCaches.Export(sealedExport);
            bool same = droppedKept > 0 && droppedKept == droppedSealed && SameExport(keptExport, sealedExport);
            for (const std::uint32_t time : SealPath(keptCaches.Stats().firstTimeMs, keptCaches.Stats().lastTimeMs))
            {
                const bool okA = keptCaches.Sample(time, a);
                const bool okB = sealedCaches.Sample(time, b);
                same = same && okA && okB && SameSample(a, b);
            }
            Check(same, "sealed track: DropBefore drops the same chunks from a sealed track, and the rest samples the same");
        }

        // An Append after Seal: a new chunk with a full keyframe, the same
        // bytes UpdateLayout with the same layout writes, decodable.
        {
            PoseTrack appended = pristine;
            appended.Seal();
            PoseTrack relayout = pristine;
            const SkeletonLayout sameLayout = relayout.Layout();
            relayout.UpdateLayout(sameLayout);
            bool sameEncoded = true;
            for (std::uint32_t frame = kRecorded; frame < kRecorded + kAppended; ++frame)
            {
                const EncodeStats x = append(appended, frame);
                const EncodeStats y = append(relayout, frame);
                sameEncoded = sameEncoded && x.entries == y.entries && x.bytes == y.bytes &&
                    x.deltaChannels == y.deltaChannels && x.rawSameChannels == y.rawSameChannels &&
                    x.rawSameFrame == y.rawSameFrame;
                if (frame == kRecorded)
                {
                    Check(!appended.Sealed() && appended.EncoderBytes() > 0,
                        "sealed track: an Append unseals the track and Begins its encoder");
                }
            }
            TrackExport appendedExport;
            TrackExport relayoutExport;
            appended.Export(appendedExport);
            relayout.Export(relayoutExport);
            const std::size_t opened = before.chunks.size();
            bool fullKeyframe = appendedExport.chunks.size() == opened + 2; // the new chunk, then one at 8 s
            if (fullKeyframe)
            {
                const TrackChunkExport& chunk = appendedExport.chunks[opened];
                std::uint16_t flags = 0;
                std::memcpy(&flags, chunk.bytes.data() + sizeof(std::uint32_t) * 2, sizeof(flags));
                fullKeyframe = chunk.anchor == -1 && (flags & kFrameKeyframe) != 0 &&
                    (flags & kFrameRelativeKeyframe) == 0 && chunk.frameTimes.front() == frameTime(kRecorded) &&
                    appendedExport.chunks[opened + 1].anchor >= 0;
            }
            Check(fullKeyframe && appended.Stats().keyframes == pristine.Stats().keyframes + 2,
                "sealed track: the first Append after Seal opens a new chunk with a full keyframe (the same 2 s block)");
            Check(sameEncoded && SameChunks(appendedExport, relayoutExport) &&
                    SameStats(appended.Stats(), relayout.Stats()),
                "sealed track: an Append after Seal writes what UpdateLayout with the same layout writes");
            PoseTrack reimported;
            std::string error;
            Check(reimported.Import(appendedExport, error), "sealed track: the track appended after Seal imports (every record decodes)");
            float worst = 0.0f;
            bool sameAppended = true;
            for (std::uint32_t frame = kRecorded; frame < kRecorded + kAppended; ++frame)
            {
                const bool okA = appended.Sample(frameTime(frame), a);
                const bool okB = relayout.Sample(frameTime(frame), b);
                sameAppended = sameAppended && okA && okB && SameSample(a, b);
                float rotation = 0.0f;
                worst = std::fmax(worst, MaxObjectError(recorded.objects[frame], a.objects, rotation));
            }
            Check(sameAppended && worst < 0.01f,
                "sealed track: the frames appended after Seal decode to the recorded poses, as UpdateLayout's do");
        }

        // PoseEncoder::Release, then Begin: what a fresh encoder encodes.
        {
            PoseEncoder fresh;
            PoseEncoder reused;
            fresh.Begin(recorded.layout, config);
            reused.Begin(demoted, config);
            std::vector<std::uint8_t> scratch;
            FrameInput input;
            input.world = world;
            for (std::uint32_t frame = 0; frame < 12; ++frame)
            {
                input.timeMs = frameTime(frame);
                input.locals = recorded.locals[frame + 100].data();
                input.objects = recorded.objects[frame + 100].data();
                reused.Encode(input, frame == 0 ? KeyframeKind::Full : KeyframeKind::None, scratch);
            }
            reused.Release();
            Check(reused.StateBytes() == 0 && !reused.HasAnchor() && fresh.StateBytes() > 0,
                "sealed track: PoseEncoder::Release frees the channel state");
            reused.Begin(recorded.layout, config);
            std::vector<std::uint8_t> fromFresh;
            std::vector<std::uint8_t> fromReused;
            for (std::uint32_t frame = 0; frame < 90; ++frame)
            {
                const KeyframeKind kind = frame == 0 ? KeyframeKind::Full
                    : (frame % 30 == 0 ? KeyframeKind::Relative : KeyframeKind::None);
                input.timeMs = frameTime(frame);
                input.locals = recorded.locals[frame].data();
                input.objects = recorded.objects[frame].data();
                fresh.Encode(input, kind, fromFresh);
                reused.Encode(input, kind, fromReused);
            }
            Check(!fromFresh.empty() && fromFresh == fromReused,
                "sealed track: an encoder Begun after Release encodes what a fresh one encodes");
        }

        // Import: sealed, no encoder state, no decoder channels until the
        // first Sample; a track that played and is imported over, or Begun
        // again, starts without caches too.
        {
            PoseTrack imported;
            std::string error;
            const bool ok = imported.Import(before, error);
            Check(ok && imported.Sealed() && imported.EncoderBytes() == 0 && imported.PlaybackCacheBytes() == 0,
                "sealed track: Import leaves the track sealed, without encoder state or decoder channels");
            TrackExport roundTrip;
            imported.Export(roundTrip);
            Check(SameExport(before, roundTrip), "sealed track: an imported track exports what it imported");
            imported.Sample(1000u, a);
            Check(imported.PlaybackCacheBytes() > 0 && imported.EncoderBytes() == 0,
                "sealed track: the first Sample of an imported track allocates its decoders");
            PoseTrack reused = imported;
            Check(reused.Import(before, error) && reused.PlaybackCacheBytes() == 0,
                "sealed track: Import over a track that played frees its playback caches");
            reused.Sample(1000u, a);
            reused.Begin(recorded.layout, config);
            Check(!reused.Sealed() && reused.EncoderBytes() > 0 && reused.PlaybackCacheBytes() == 0,
                "sealed track: Begin over a track that played frees its playback caches");
        }

        // A track sealed before its first frame appends what a fresh one does.
        {
            PoseTrack empty;
            PoseTrack fresh;
            empty.Begin(recorded.layout, config);
            fresh.Begin(recorded.layout, config);
            empty.Seal();
            Check(!empty.Sample(0u, a) && empty.Sealed() && empty.EncoderBytes() == 0,
                "sealed track: an empty sealed track samples nothing and holds no encoder state");
            for (std::uint32_t frame = 0; frame < 40; ++frame)
            {
                append(empty, frame);
                append(fresh, frame);
            }
            TrackExport emptyExport;
            TrackExport freshExport;
            empty.Export(emptyExport);
            fresh.Export(freshExport);
            Check(!emptyExport.chunks.empty() && SameExport(emptyExport, freshExport),
                "sealed track: a track sealed before its first frame records what a fresh track records");
        }

        std::printf("  sealed track (%u channels, %zu chunks): %zu samples compared, sealed once and every %zu samples "
                    "(%zu seals), %zu differ; encoder %zu -> 0 bytes, playback caches %zu -> 0 bytes (%.0f and %.0f "
                    "bytes per channel)\n",
            channels, before.chunks.size(), path.size(), kSealEvery, seals, differOnce + differResealed, encoderBytes,
            cacheBytes, static_cast<double>(encoderBytes) / channels, static_cast<double>(cacheBytes) / channels);
    }

    void TestBudgetDropsOldestChunk()
    {
        Recorded recorded = MakeRecording(90);
        StreamConfig config;
        config.keyframeIntervalMs = 330;
        config.byteBudget = 8u * 1024u;
        PoseTrack track;
        track.Begin(recorded.layout, config);
        for (std::uint32_t frame = 0; frame < 90; ++frame)
        {
            FrameInput input;
            input.timeMs = frame * 33u;
            input.locals = recorded.locals[frame].data();
            input.objects = recorded.objects[frame].data();
            track.Append(input);
        }
        Check(track.Stats().droppedChunks > 0, "budget drops old chunks");
        Check(track.Stats().bytes <= config.byteBudget + 4096u,
            "retained bytes stay near the budget");
        PoseSample sample;
        Check(track.Sample(0, sample) && sample.frameTimeA == track.Stats().firstTimeMs,
            "sampling before the retained window clamps to the oldest keyframe");
    }

    // QA 224: the capture's tracks take the recorder's bound
    // (pose_capture_rules.h kCaptureTrackByteBudget), not a budget of their
    // own: the same frames that make an 8 KiB track drop its start keep every
    // chunk, and the first frame stays the record's first.
    void TestCaptureBudgetKeepsTheStart()
    {
        Recorded recorded = MakeRecording(90);
        StreamConfig small;
        small.keyframeIntervalMs = 330;
        small.byteBudget = 8u * 1024u;
        StreamConfig capture = small;
        capture.byteBudget = replay::pose::capture::kCaptureTrackByteBudget;
        PoseTrack trimmed, kept;
        trimmed.Begin(recorded.layout, small);
        kept.Begin(recorded.layout, capture);
        for (std::uint32_t frame = 0; frame < 90; ++frame)
        {
            FrameInput input;
            input.timeMs = 1000u + frame * 33u;
            input.locals = recorded.locals[frame].data();
            input.objects = recorded.objects[frame].data();
            trimmed.Append(input);
            kept.Append(input);
        }
        Check(trimmed.Stats().droppedChunks > 0 && trimmed.Stats().firstTimeMs > 1000u,
            "capture budget: the small budget loses the record's start (the QA 224 player)");
        Check(kept.Stats().droppedChunks == 0 && kept.Stats().firstTimeMs == 1000u &&
                kept.Stats().bytes > small.byteBudget,
            "capture budget: the capture's track keeps every chunk past the old budget");
        PoseSample sample;
        Check(kept.Sample(1000u, sample) && sample.frameTimeA == 1000u,
            "capture budget: the first frame samples at the record's start");
        Check(replay::pose::capture::kCaptureTrackByteBudget > StreamConfig{}.byteBudget,
            "capture budget: above the stream's default");
    }
}

namespace
{
    void TestScaledRigDiscovery()
    {
        const SyntheticSkeleton skeleton;
        std::vector<BoneQts> locals[4];
        std::vector<BoneQts> objects[4];
        DiscoveryFrame frames[4];
        for (int frame = 0; frame < 4; ++frame)
        {
            skeleton.Pose(0.3f * frame + 0.2f, CompositionOrder::ParentThenLocal,
                locals[frame], objects[frame], true);
            frames[frame] = {locals[frame].data(), objects[frame].data()};
        }
        SkeletonLayout layout;
        DiscoveryStats stats;
        DiscoverSkeletonLayout(frames, 4, skeleton.Count(), {}, layout, stats);
        Check(layout.inheritScale, "a scaled rig selects scale-inheriting composition");
        Check(stats.unresolved == 0, "a scaled rig resolves every bone");
        const CompositionError error =
            MeasureCompositionError(layout, locals[2].data(), objects[2].data());
        Check(error.maxTranslation < 0.0005f, "scaled rig rebuilds object translations");
    }

    // A zero-offset helper sits exactly on its parent, so its child matches
    // both. The closest preceding index (the helper) must win.
    void TestCoLocatedHelperTieBreak()
    {
        std::vector<std::int32_t> parent = {kRootParent, 0, 1, 2};
        std::vector<BoneQts> bind(4);
        bind[1].translation[2] = 0.5f; // spine
        // bone 2: helper with identity local (co-located with spine)
        bind[3].translation[0] = 0.3f; // child of helper
        std::vector<BoneQts> locals[3];
        std::vector<BoneQts> objects[3];
        DiscoveryFrame frames[3];
        for (int frame = 0; frame < 3; ++frame)
        {
            locals[frame] = bind;
            locals[frame][0].rotation = AxisAngle(0.0f, 0.0f, 1.0f, 0.4f * frame);
            locals[frame][1].rotation = AxisAngle(1.0f, 0.0f, 0.0f, 0.3f * frame + 0.1f);
            locals[frame][3].rotation = AxisAngle(0.0f, 1.0f, 0.0f, 0.2f * frame);
            objects[frame].resize(4);
            objects[frame][0] = locals[frame][0];
            for (int bone = 1; bone < 4; ++bone)
                objects[frame][bone] = Compose(objects[frame][parent[bone]], locals[frame][bone],
                    CompositionOrder::ParentThenLocal);
            frames[frame] = {locals[frame].data(), objects[frame].data()};
        }
        SkeletonLayout layout;
        DiscoveryStats stats;
        DiscoverSkeletonLayout(frames, 3, 4, {}, layout, stats);
        Check(layout.ambiguous[3] == 1, "child of a co-located helper is flagged ambiguous");
        Check(layout.parent[3] == 2, "tie-break picks the closest preceding parent");
    }

    void TestRuntimeDemotionAndLayoutSwitch()
    {
        Recorded recorded = MakeRecording(90);
        SkeletonLayout wrong = recorded.layout;
        const std::uint32_t victim = 20;
        const std::int32_t real = wrong.parent[victim];
        wrong.parent[victim] = real == 1 ? 2 : 1; // deliberately wrong parent
        const std::vector<std::uint32_t> failing = FindFailingBones(
            wrong, recorded.locals[30].data(), recorded.objects[30].data(), 0.002f, 0.004f);
        Check(failing.size() == 1 && failing[0] == victim,
            "runtime check blames only the mis-parented bone");
        DemoteBones(wrong, failing);
        Check(wrong.parent[victim] == kUnresolvedParent && wrong.unresolvedBones.size() == 1,
            "demotion records the bone in object space");
        Check(FindFailingBones(wrong, recorded.locals[30].data(),
                  recorded.objects[30].data(), 0.002f, 0.004f).empty(),
            "no failures remain after demotion");

        StreamConfig config;
        config.keyframeIntervalMs = 100000; // only the layout switch starts a chunk
        PoseTrack track;
        track.Begin(recorded.layout, config);
        for (std::uint32_t frame = 0; frame < 90; ++frame)
        {
            if (frame == 45)
                track.UpdateLayout(wrong);
            FrameInput input;
            input.timeMs = frame * 33u;
            input.locals = recorded.locals[frame].data();
            input.objects = recorded.objects[frame].data();
            track.Append(input);
        }
        Check(track.Stats().keyframes == 2, "a layout switch forces a keyframe chunk");
        PoseSample sample;
        float worst = 0.0f;
        bool sampled = true;
        for (std::uint32_t time = 40u * 33u; time <= 50u * 33u; time += 11u)
        {
            sampled = sampled && track.Sample(time, sample);
            const std::uint32_t frame = time / 33u;
            float rotation = 0.0f;
            if (time % 33u == 0)
                worst = std::fmax(worst, MaxObjectError(recorded.objects[frame], sample.objects, rotation));
        }
        Check(sampled && worst < 0.01f, "sampling across a layout switch stays accurate");
    }
}

namespace
{
    void TestBasisRoundTrip()
    {
        std::mt19937 random(99);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        float worst = 0.0f;
        for (int i = 0; i < 2000; ++i)
        {
            const Quat q = Normalize({unit(random), unit(random), unit(random), unit(random)});
            Vec3 right, forward, up;
            BasisFromQuat(q, right, forward, up);
            worst = std::fmax(worst, AngleBetween(q, QuatFromBasis(right, forward, up)));
        }
        Check(worst < 0.0005f, "basis <-> quaternion round trip");
    }
}

namespace
{
    // Raw lanes logged live by a sector probe.
    void TestDecodeWorldMatrixLiveSamples()
    {
        struct Sample
        {
            std::uint32_t lanes[16];
            float worldX, worldY, worldZ;
        };
        const Sample samples[] = {
            {{0xbf629298, 0x3eee55a9, 0, 0, 0xbeee55a9, 0xbf629298, 0, 0xb3800000,
                 0, 0, 0x3f800000, 0xb3800000, 0xc07e4200, 0xc1c4a6c0, 0x42da712d, 0xffd0ffc5},
                -1891.973f, -1560.581f, 109.221f},
            {{0xBF4CD540, 0xBF198E55, 0, 0, 0x3F198E55, 0xBF4CD540, 0, 0xB4000000,
                 0, 0, 0x3F800000, 0xB4000000, 0xC1930A40, 0xC0E38800, 0x42D8B6E1, 0xFFCFFFC6},
                -1874.380f, -1575.110f, 108.357f},
        };
        bool positions = true;
        bool rotations = true;
        for (const Sample& sample : samples)
        {
            float matrix[16];
            std::memcpy(matrix, sample.lanes, sizeof(matrix));
            WorldTransform world;
            if (!DecodeWorldMatrix(matrix, world))
            {
                positions = false;
                continue;
            }
            positions = positions && std::fabs(world.position.x - sample.worldX) < 0.002f &&
                std::fabs(world.position.y - sample.worldY) < 0.002f &&
                std::fabs(world.position.z - sample.worldZ) < 0.002f;
            Vec3 right, forward, up;
            BasisFromQuat(world.rotation, right, forward, up);
            rotations = rotations && std::fabs(forward.x - matrix[4]) < 0.001f &&
                std::fabs(forward.y - matrix[5]) < 0.001f && std::fabs(up.z - matrix[10]) < 0.001f;
        }
        Check(positions, "sector-relative engine matrix decodes to the native world position");
        Check(rotations, "engine matrix rotation survives the quaternion round trip");

        float empty[16] = {};
        WorldTransform unused;
        Check(!DecodeWorldMatrix(empty, unused), "an empty matrix is not a transform");
    }
}

namespace
{
    void TestRebaseDetection()
    {
        const auto probe = [](float ox, float rx, std::uint32_t timeMs) {
            RebaseProbe p;
            p.world.position = {ox, 0.0f, 0.0f};
            p.root = {rx, 0.0f, 0.0f};
            p.timeMs = timeMs;
            return p;
        };
        Check(!IsRebase(probe(0.0f, 0.0f, 0), probe(0.5f, 0.5f, 33)),
            "origin and root moving together is motion, not a rebase");
        Check(IsRebase(probe(0.0f, 0.0f, 0), probe(1.0f, 0.05f, 33)),
            "origin moving without the root is a rebase (ragdoll exit)");
        Check(IsRebase(probe(0.0f, 0.0f, 0), probe(10.0f, 10.0f, 33)),
            "origin and root jumping together is a teleport");
        Check(!IsRebase(probe(0.0f, 0.0f, 0), probe(1.0f, 0.05f, 500)),
            "frames far apart are not compared");
        Check(!IsRebase(probe(0.0f, 0.0f, 33), probe(1.0f, 0.05f, 33)),
            "same time is not compared");

        // The root world position composes the entity rotation.
        WorldTransform world;
        world.position = {10.0f, 0.0f, 0.0f};
        world.rotation = AxisAngle(0.0f, 0.0f, 1.0f, 3.14159265f * 0.5f);
        BoneQts root;
        root.translation[0] = 1.0f;
        const Vec3 at = RootWorldPosition(world, root);
        Check(std::fabs(at.x - 10.0f) < 1e-4f && std::fabs(at.y - 1.0f) < 1e-4f,
            "root world position rotates the object translation");

        // A flagged frame holds the earlier one until its own time.
        Recorded recorded = MakeRecording(12); // discovery reads frames 0, 5 and 11
        StreamConfig config;
        PoseTrack track;
        track.Begin(recorded.layout, config);
        float matrix[16] = {};
        for (std::uint32_t frame = 0; frame < 3; ++frame)
        {
            FrameInput input;
            input.timeMs = frame * 33u;
            input.flags = frame == 2 ? kFrameNoInterpolation : 0;
            input.world = matrix;
            input.locals = recorded.locals[frame].data();
            input.objects = recorded.objects[frame].data();
            track.Append(input);
        }
        PoseSample sample;
        Check(track.Sample(16, sample) && sample.t > 0.4f, "an unflagged pair interpolates");
        Check(track.Sample(33 + 16, sample) && sample.t == 0.0f && sample.frameTimeA == 33,
            "a flagged later frame disables interpolation into it");
    }

    // The heading rebase (cover): the entity turns about 180 degrees in one
    // frame and the root bone turns back, the mesh stays. Numbers from a
    // recorded shootout (Arthur's record 0x6C8).
    void TestHeadingRebase()
    {
        constexpr float kDeg = 3.14159265f / 180.0f;
        const auto probe = [&](float originX, float entityYawDeg, float rootWorldYawDeg, float rootX, std::uint32_t timeMs) {
            RebaseProbe p;
            p.world.position = {originX, 0.0f, 0.0f};
            p.world.rotation = AxisAngle(0.0f, 0.0f, 1.0f, entityYawDeg * kDeg);
            p.root = {rootX, 0.0f, 0.0f};
            p.rootRotation = AxisAngle(0.0f, 0.0f, 1.0f, rootWorldYawDeg * kDeg);
            p.hasRootRotation = true;
            p.timeMs = timeMs;
            return p;
        };
        Check(IsRebase(probe(0.0f, 143.9f, 106.0f, 0.0f, 29126), probe(0.025f, -62.4f, 96.9f, 0.106f, 29160)),
            "cover flip at 29126 ms (entity 154 degrees, root 9) is a rebase");
        Check(IsRebase(probe(0.0f, -62.4f, -0.2f, 0.0f, 31819), probe(0.0f, 117.6f, -0.4f, 0.001f, 31852)),
            "exact 180 degree flip with the root still is a rebase");
        Check(!IsRebase(probe(0.0f, 0.0f, 0.0f, 0.0f, 0), probe(0.0f, 30.0f, 30.0f, 0.0f, 33)),
            "turning in place (the root follows) is motion");
        Check(!IsRebase(probe(0.0f, 0.0f, 0.0f, 0.0f, 0), probe(0.0f, 50.0f, 40.0f, 0.0f, 33)),
            "a fast turn with the root lagging 10 degrees is motion");
        Check(!IsRebase(probe(0.0f, -62.4f, -0.2f, 0.0f, 31819), probe(0.0f, 117.6f, -0.4f, 0.0f, 32319)),
            "frames 500 ms apart are not compared");
        RebaseProbe noRotation = probe(0.0f, -62.4f, -0.2f, 0.0f, 31819);
        noRotation.hasRootRotation = false;
        Check(!IsRebase(noRotation, probe(0.0f, 117.6f, -0.4f, 0.0f, 31852)),
            "no root rotation on one side: only the position rules");

        // MakeRebaseProbe composes the root's world rotation.
        WorldTransform world;
        world.rotation = AxisAngle(0.0f, 0.0f, 1.0f, 180.0f * kDeg);
        BoneQts root;
        root.rotation = AxisAngle(0.0f, 0.0f, 1.0f, -180.0f * kDeg);
        const RebaseProbe made = MakeRebaseProbe(world, root, 5);
        Check(made.hasRootRotation && made.timeMs == 5 && AngleBetween(made.rootRotation, Quat{0.0f, 0.0f, 0.0f, 1.0f}) < 1e-3f,
            "root world rotation is the entity rotation times the root object rotation");

        // Track level: an unflagged flip (a clip recorded before the rule)
        // is held at playback, not blended through 180 degrees.
        Recorded recorded = MakeRecording(12);
        StreamConfig config;
        PoseTrack track;
        track.Begin(recorded.layout, config);
        const Quat back = AxisAngle(0.0f, 0.0f, 1.0f, -180.0f * kDeg);
        std::vector<std::vector<BoneQts>> objects(4);
        for (std::uint32_t frame = 0; frame < 4; ++frame)
        {
            const bool flipped = frame >= 2;
            std::vector<BoneQts> locals = recorded.locals[frame];
            if (flipped)
            {
                // The object space turns back by the entity's 180 degrees.
                BoneQts& rootLocal = locals[0];
                const Vec3 moved = Rotate(back, {rootLocal.translation[0], rootLocal.translation[1], rootLocal.translation[2]});
                rootLocal.translation[0] = moved.x;
                rootLocal.translation[1] = moved.y;
                rootLocal.translation[2] = moved.z;
                rootLocal.rotation = Normalize(Multiply(back, rootLocal.rotation));
            }
            objects[frame].assign(locals.size(), BoneQts{});
            for (std::size_t bone = 0; bone < locals.size(); ++bone)
            {
                const std::int32_t parent = recorded.skeleton.parent[bone];
                objects[frame][bone] = parent < 0 ? locals[bone]
                    : Compose(objects[frame][static_cast<std::size_t>(parent)], locals[bone], CompositionOrder::ParentThenLocal, false);
            }
            float matrix[16] = {};
            matrix[0] = flipped ? -1.0f : 1.0f;  // right
            matrix[5] = flipped ? -1.0f : 1.0f;  // forward
            matrix[10] = 1.0f;                   // up
            FrameInput input;
            input.timeMs = frame * 33u;
            input.world = matrix;
            input.locals = locals.data();
            input.objects = objects[frame].data();
            track.Append(input);
        }
        PoseSample held;
        Check(track.Sample(33 + 16, held) && held.t == 0.0f && held.frameTimeA == 33 && track.UnflaggedRebaseHolds() == 1,
            "an unflagged heading flip holds the earlier frame");
        float rotation = 0.0f;
        Check(held.objects.size() == objects[1].size() && MaxObjectError(objects[1], held.objects, rotation) < 0.01f,
            "the held pose is the earlier frame's, not a folded blend");
        PoseSample after;
        Check(track.Sample(66 + 16, after) && after.t > 0.4f && after.t < 0.6f,
            "two frames after the flip blend normally");
    }
}

int RunPoseSampleTests(); // tests/pose_sample_tests.cpp
int RunPoseCaptureTests(); // tests/pose_capture_tests.cpp

int RunPoseStreamTests()
{
    TestRebaseDetection();
    TestHeadingRebase();
    TestDecodeWorldMatrixLiveSamples();
    TestBasisRoundTrip();
    TestScaledRigDiscovery();
    TestCoLocatedHelperTieBreak();
    TestRuntimeDemotionAndLayoutSwitch();
    TestQuaternionPacking();
    TestLayoutDiscovery(CompositionOrder::ParentThenLocal, "discovery accepts parent-then-local frames");
    TestLayoutDiscovery(CompositionOrder::LocalThenParent, "discovery accepts local-then-parent frames");
    TestStreamRoundTrip();
    TestStaticBonesCostNothing();
    TestSampleCacheMatchesFreshDecode();
    TestBackwardScrubCheckpoints();
    TestLayoutReuseAcrossPeds();
    TestReparentAfterContradiction();
    TestFailingBonesChanged();
    TestValidationBand();
    TestRelativeKeyframes();
    TestLayoutFromEngineParents();
    TestCompareLatePose();
    TestCompareWrittenPose();
    TestDecoderRejectsBadInput();
    TestBudgetDropsOldestChunk();
    TestCaptureBudgetKeepsTheStart();
    TestExportSharesChunks();
    TestSealedTrack();
    return g_poseFailures + RunPoseSampleTests() + RunPoseCaptureTests();
}
