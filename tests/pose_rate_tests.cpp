// Pose rate (pose_rate_check.h): a recorded animation updated at the capture
// rate against one the engine updated every 2nd or 3rd frame (stair steps),
// and the one frame flips, as ReplayClipTool pose-rate counts them; and the
// deadband smoothing of the pose codec (pose_deadband_smooth.h).
#include "pose_rate_check.h"
#include "pose_deadband_smooth.h"
#include "pose_skeleton_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_poseRateFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_poseRateFailures;
    }

    namespace pr = replay::poserate;
    using replay::pose::BoneQts;

    BoneQts AxisZ(float rad)
    {
        BoneQts bone;
        bone.rotation.x = 0.0f;
        bone.rotation.y = 0.0f;
        bone.rotation.z = std::sin(rad * 0.5f);
        bone.rotation.w = std::cos(rad * 0.5f);
        return bone;
    }

    // `frames` frames of one bone turning `radPerFrame` each capture frame,
    // the engine updating it every `every` frames (1: every frame).
    pr::Counts Measure(std::size_t frames, float radPerFrame, std::size_t every, std::uint32_t stepMs = 33)
    {
        std::vector<BoneQts> track;
        for (std::size_t n = 0; n < frames; ++n)
            track.push_back(AxisZ(radPerFrame * static_cast<float>(n - n % every)));
        pr::Counts counts;
        for (std::size_t n = 2; n < frames; ++n)
        {
            const std::uint32_t t = static_cast<std::uint32_t>(n) * stepMs;
            pr::MeasureTriple(&track[n - 2], &track[n - 1], &track[n], 1, t - 2 * stepMs, t - stepMs, t, counts);
        }
        return counts;
    }

    void TestRate()
    {
        const pr::Counts full = Measure(60, 0.02f, 1);
        Check(full.triples == 58 && full.moving == 58 && full.held == 0, "a bone updated every frame holds nothing");
        Check(!full.Stairs(), "full rate is not stair stepped");

        const pr::Counts half = Measure(60, 0.02f, 2);
        Check(half.moving == 58 && half.held == 58, "updated every 2nd frame: every moving triple holds");
        Check(half.Stairs() && half.HeldPercent() == 100.0f, "half rate is stair stepped");

        const pr::Counts third = Measure(61, 0.02f, 3);
        // A A A triples do not move (span 0); A A C and A C C hold.
        Check(third.moving > 0 && third.held == third.moving && third.moving < third.triples,
            "updated every 3rd frame: the moving triples all hold, the still ones are not counted");

        const pr::Counts slow = Measure(60, 0.004f, 1);
        Check(slow.moving == 0, "a bone turning under the span threshold over two frames is not counted");

        const pr::Counts gap = Measure(10, 0.02f, 1, 150);
        Check(gap.triples == 0, "frames further apart than kMaxStepMs are a hole, not a step");
    }

    void TestFlip()
    {
        std::vector<BoneQts> track = {AxisZ(0.0f), AxisZ(0.2f), AxisZ(0.01f)};
        pr::Counts counts;
        pr::MeasureTriple(&track[0], &track[1], &track[2], 1, 0, 33, 66, counts);
        Check(counts.flips == 1 && counts.flipFrames == 1, "a one frame excursion is a flip");

        std::vector<BoneQts> turn = {AxisZ(0.0f), AxisZ(0.2f), AxisZ(0.4f)};
        pr::Counts steady;
        pr::MeasureTriple(&turn[0], &turn[1], &turn[2], 1, 0, 33, 66, steady);
        Check(steady.flips == 0 && steady.held == 0 && steady.moving == 1, "a steady turn is neither a flip nor held");

        std::vector<BoneQts> small = {AxisZ(0.0f), AxisZ(0.03f), AxisZ(0.0f)};
        pr::Counts tiny;
        pr::MeasureTriple(&small[0], &small[1], &small[2], 1, 0, 33, 66, tiny);
        Check(tiny.flips == 0, "an excursion under kFlipRad is not a flip");
    }

    void TestAdd()
    {
        pr::Counts a = Measure(60, 0.02f, 1);
        const pr::Counts b = Measure(60, 0.02f, 2);
        a.Add(b);
        Check(a.moving == 116 && a.held == 58 && a.HeldPercent() == 50.0f, "counts add up");
        pr::Counts empty;
        Check(!empty.Stairs() && empty.HeldPercent() == 0.0f, "no moving triple is not stair stepped");
    }
    namespace sm = replay::pose::smooth;

    void TestSmoothChannel()
    {
        const sm::Gate gate = sm::Gate::For(replay::pose::StreamConfig{});
        BoneQts out;
        const BoneQts still = AxisZ(0.3f);
        sm::SmoothChannel(still, still, still, gate, out);
        Check(pr::Angle(out.rotation, still.rotation) < 1e-6f, "a still channel is left as it is");

        // A deadband step: held, then one epsilon and a bit.
        sm::SmoothChannel(AxisZ(0.0f), AxisZ(0.0f), AxisZ(0.0024f), gate, out);
        const float first = pr::Angle(out.rotation, AxisZ(0.0f).rotation);
        Check(first > 0.0004f && first < 0.0008f, "a deadband step starts as a ramp (a quarter of it)");

        const BoneQts fast0 = AxisZ(0.0f), fast1 = AxisZ(0.05f), fast2 = AxisZ(0.1f);
        sm::SmoothChannel(fast0, fast1, fast2, gate, out);
        Check(pr::Angle(out.rotation, fast1.rotation) < 1e-6f, "a moving channel is not touched");

        // Turning back for one frame inside the gate (the codec's zig zag) is averaged.
        sm::SmoothChannel(AxisZ(0.0f), AxisZ(0.0025f), AxisZ(0.0f), gate, out);
        Check(pr::Angle(out.rotation, AxisZ(0.00125f).rotation) < 1e-4f, "a one frame zig zag is halved");

        // The other hemisphere of the same rotation averages as the same rotation.
        BoneQts flipped = AxisZ(0.0f);
        flipped.rotation = {-flipped.rotation.x, -flipped.rotation.y, -flipped.rotation.z, -flipped.rotation.w};
        sm::SmoothChannel(flipped, AxisZ(0.0f), AxisZ(0.0f), gate, out);
        Check(pr::Angle(out.rotation, AxisZ(0.0f).rotation) < 1e-6f, "a sign flipped neighbour is the same rotation");

        BoneQts a, b, c;
        a.translation[0] = 0.0f;
        b.translation[0] = 0.0f;
        c.translation[0] = 0.0006f;
        sm::SmoothChannel(a, b, c, gate, out);
        Check(std::fabs(out.translation[0] - 0.00015f) < 1e-6f, "a translation deadband step starts as a ramp");
        c.translation[0] = 0.01f;
        sm::SmoothChannel(a, b, c, gate, out);
        Check(out.translation[0] == 0.0f, "a moving translation is not touched");
    }

    // A three bone chain whose middle bone drifts slowly (0.0007 rad per frame):
    // the coarse codec plays it as steps of about its epsilon.
    replay::pose::TrackExport DriftTrack(std::size_t frames, std::uint32_t noInterpolationFrame, std::size_t spikeFrame = ~std::size_t{0},
        bool fine = false)
    {
        const std::uint16_t parents[3] = {0xFFFF, 0, 1};
        replay::pose::SkeletonLayout layout;
        replay::pose::DiscoveryStats stats;
        replay::pose::BuildLayoutFromParents(parents, 3, false, {}, layout, stats);
        // 64 bit rotations (a bone with a large subtree, the spine, the
        // neck): the 32 bit code's own grid (0.0014 per component) would
        // hide the deadband under its quantization.
        layout.highPrecision.assign(3, 1);
        replay::pose::PoseTrack track;
        replay::pose::StreamConfig config;
        if (fine)
        {
            config.rotationEpsilon = sm::kStandardRotationEpsilon;
            config.translationEpsilon = sm::kStandardTranslationEpsilon;
        }
        track.Begin(layout, config);
        for (std::size_t f = 0; f < frames; ++f)
        {
            BoneQts locals[3];
            locals[1] = AxisZ(0.0007f * static_cast<float>(f));
            locals[1].translation[2] = 0.5f;
            locals[2].translation[0] = 0.5f;
            BoneQts objects[3];
            replay::pose::ComposeObjects(layout, locals, nullptr, objects);
            // Cell 0 (the w lane of the translation holds the cell bits).
            float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10.0f, 20.0f, 1.0f, 0.0f};
            if (f == spikeFrame)
                world[13] += 0.066f; // the entity goes 66 mm forward for one frame
            replay::pose::FrameInput input;
            input.timeMs = static_cast<std::uint32_t>(f) * 33u;
            input.flags = f == noInterpolationFrame ? replay::pose::kFrameNoInterpolation : 0;
            input.world = world;
            input.locals = locals;
            input.objects = objects;
            track.Append(input);
        }
        replay::pose::TrackExport out;
        track.Export(out);
        return out;
    }

    // How far bone 1's steps from one recorded frame to the next stray from
    // the drift's true 0.0007 rad (RMS): the trembling the codec adds.
    float StepNoise(const replay::pose::TrackExport& exported, std::size_t frames)
    {
        replay::pose::PoseTrack track;
        std::string error;
        if (!track.Import(exported, error))
            return -1.0f;
        replay::pose::PoseSample sample;
        double sum = 0.0;
        std::size_t count = 0;
        replay::pose::Quat previous{};
        for (std::size_t f = 0; f < frames; ++f)
        {
            if (!track.Sample(static_cast<std::uint32_t>(f) * 33u, sample))
                return -1.0f;
            if (f > 0)
            {
                const double d = pr::Angle(previous, sample.locals[1].rotation) - 0.0007;
                sum += d * d;
                ++count;
            }
            previous = sample.locals[1].rotation;
        }
        return count ? static_cast<float>(std::sqrt(sum / static_cast<double>(count))) : -1.0f;
    }

    void TestSmoothTrack()
    {
        constexpr std::size_t kFrames = 150; // 5 s: keyframes and relative keyframes
        const replay::pose::TrackExport coarse = DriftTrack(kFrames, 0xFFFFFFFFu);
        Check(sm::NeedsSmoothing(coarse.config), "a track of the coarse codec needs the smoothing");
        const float coarseNoise = StepNoise(coarse, kFrames);
        Check(coarseNoise > 0.0006f, "the coarse codec plays a slow drift in deadband steps");

        replay::pose::TrackExport smoothed;
        sm::SmoothStats stats;
        std::string error;
        Check(sm::SmoothTrack(coarse, smoothed, stats, error), "the coarse track smooths");
        Check(stats.frames == kFrames && stats.smoothedFrames == kFrames - 2, "every frame kept, the inner ones smoothed");
        Check(!sm::NeedsSmoothing(smoothed.config) && smoothed.config.rotationEpsilon == sm::kStandardRotationEpsilon,
            "the smoothed track has the fine codec (smoothing it again is a no-op)");
        std::size_t smoothedFrames = 0;
        for (const auto& chunk : smoothed.chunks)
            smoothedFrames += chunk.frameTimes.size();
        Check(smoothedFrames == kFrames, "the frame times are kept");
        const float smoothNoise = StepNoise(smoothed, kFrames);
        std::printf("deadband drift: step noise %.5f rad coarse, %.5f smoothed\n", coarseNoise, smoothNoise);
        Check(smoothNoise >= 0.0f && smoothNoise < 0.6f * coarseNoise, "the smoothed drift strays about half as far from its true steps");

        std::vector<replay::pose::PoseRecordExport> records(1);
        records[0].recordId = 7;
        records[0].boneCount = 3;
        records[0].track = smoothed;
        sm::RecordsSummary summary;
        sm::SmoothRecords(records, summary);
        Check(summary.records == 1 && summary.smoothed == 0, "a fine track is left as it is");
        records[0].track = coarse;
        sm::SmoothRecords(records, summary);
        Check(summary.smoothed == 1 && summary.failed == 0 && !sm::NeedsSmoothing(records[0].track.config),
            "SmoothRecords smooths the coarse records in place");

        // A cut (no interpolation into frame 60): kept, and not smoothed across.
        const replay::pose::TrackExport cut = DriftTrack(kFrames, 60);
        replay::pose::TrackExport cutSmoothed;
        Check(sm::SmoothTrack(cut, cutSmoothed, stats, error), "a track with a cut smooths");
        Check(stats.smoothedFrames == kFrames - 4, "the frames on either side of the cut are not smoothed");
        replay::pose::PoseTrack played;
        Check(played.Import(cutSmoothed, error), "the smoothed track imports");
        replay::pose::PoseSample sample;
        Check(played.Sample(59u * 33u + 16u, sample) && sample.t == 0.0f, "the cut still holds the frame before it");
    }

    replay::pose::smooth::WorldFrame Still(std::uint32_t t, float x)
    {
        replay::pose::smooth::WorldFrame frame;
        frame.timeMs = t;
        const float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 5.0f, 1.0f, 0.0f};
        for (int n = 0; n < 16; ++n)
            frame.world[n] = world[n];
        return frame;
    }

    void TestWorldSpikes()
    {
        std::vector<sm::WorldFrame> frames;
        for (std::uint32_t f = 0; f < 10; ++f)
            frames.push_back(Still(f * 33u, 1.0f));
        frames[5] = Still(5u * 33u, 1.066f);
        std::vector<std::size_t> spikes = sm::FindWorldSpikes(frames);
        Check(spikes.size() == 1 && spikes[0] == 5, "a one frame jump that comes back is a spike");

        std::vector<sm::WorldFrame> step = frames;
        for (std::size_t f = 5; f < step.size(); ++f)
            step[f] = Still(static_cast<std::uint32_t>(f) * 33u, 1.066f);
        Check(sm::FindWorldSpikes(step).empty(), "a jump that stays is not a spike");

        std::vector<sm::WorldFrame> cut = frames;
        cut[5].flags = replay::pose::kFrameNoInterpolation;
        Check(sm::FindWorldSpikes(cut).empty(), "a frame after a cut is not a spike");

        std::vector<sm::WorldFrame> walking;
        for (std::uint32_t f = 0; f < 10; ++f)
            walking.push_back(Still(f * 33u, 1.0f + 0.04f * static_cast<float>(f)));
        walking[5] = Still(5u * 33u, 1.0f + 0.04f * 4.0f);
        Check(sm::FindWorldSpikes(walking).empty(), "a moving body is never repaired (the rule wants still frames around)");

        std::vector<sm::WorldFrame> gap = frames;
        for (std::size_t f = 6; f < gap.size(); ++f)
            gap[f].timeMs += 500;
        Check(sm::FindWorldSpikes(gap).empty(), "frames far apart in time are not a spike");

        // On a coarse track, the smoothing pass repairs it with the bones.
        const replay::pose::TrackExport coarse = DriftTrack(60, 0xFFFFFFFFu, 30);
        Check(sm::FindWorldSpikes(sm::WorldFrames(coarse)).size() == 1, "the recorded spike is found in the records");
        replay::pose::TrackExport repaired;
        sm::SmoothStats stats;
        std::string error;
        Check(sm::SmoothTrack(coarse, repaired, stats, error) && stats.worldSpikes == 1 &&
                sm::FindWorldSpikes(sm::WorldFrames(repaired)).empty(),
            "the smoothing pass repairs the spike");

        // On a fine track, only the spike: the bones and the codec are kept.
        std::vector<replay::pose::PoseRecordExport> records(1);
        records[0].recordId = 9;
        records[0].boneCount = 3;
        records[0].track = DriftTrack(60, 0xFFFFFFFFu, 30, true);
        const replay::pose::TrackExport before = records[0].track;
        sm::RecordsSummary summary;
        sm::SmoothRecords(records, summary);
        Check(summary.spikeRecords == 1 && summary.worldSpikes == 1 && summary.smoothed == 0,
            "a fine track with a spike is encoded again for the spike alone");
        Check(sm::FindWorldSpikes(sm::WorldFrames(records[0].track)).empty() &&
                records[0].track.config.rotationEpsilon == sm::kStandardRotationEpsilon,
            "the spike is gone and the codec kept");
        replay::pose::PoseTrack a, b;
        replay::pose::PoseSample sa, sb;
        bool same = a.Import(before, error) && b.Import(records[0].track, error);
        for (std::uint32_t f = 0; f < 60 && same; ++f)
            same = a.Sample(f * 33u, sa) && b.Sample(f * 33u, sb) &&
                pr::Angle(sa.locals[1].rotation, sb.locals[1].rotation) < 1e-6f;
        Check(same, "the bones of a spike only pass are the recorded ones");
        sm::SmoothRecords(records, summary);
        Check(summary.spikeRecords == 0 && summary.smoothed == 0, "a second pass changes nothing");
    }

}

int RunPoseRateTests()
{
    TestRate();
    TestFlip();
    TestAdd();
    TestSmoothChannel();
    TestSmoothTrack();
    TestWorldSpikes();
    if (g_poseRateFailures == 0)
        std::puts("pose rate tests passed.");
    return g_poseRateFailures;
}
