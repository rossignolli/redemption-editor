// Frozen poses (frozen_pose_check.h): a walking ped whose recorded pose
// repeats from one capture frame to the next, split by the game camera's view,
// and the verdict of ReplayClipTool frozen-poses.
#include "frozen_pose_check.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    int g_frozenPoseFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_frozenPoseFailures;
    }

    namespace fp = replay::frozenpose;

    replay::pose::Quat AxisZ(float rad)
    {
        replay::pose::Quat q;
        q.z = std::sin(rad * 0.5f);
        q.w = std::cos(rad * 0.5f);
        return q;
    }

    // A camera at the origin looking down +Y (heading 0), 50 degrees vertical.
    fp::View NorthCamera()
    {
        fp::View view;
        view.fovDeg = 50.0f;
        view.valid = true;
        return view;
    }

    // A ped walking along +X at `speed` m/s, one frame every 33 ms from
    // (x0, y), with frames listed in `frozen` repeating the pose before.
    std::vector<fp::Frame> Walk(float x0, float y, float speed, int count, const std::vector<int>& frozen)
    {
        std::vector<fp::Frame> frames;
        for (int n = 0; n < count; ++n)
        {
            fp::Frame f;
            f.timeMs = 1000u + 33u * static_cast<std::uint32_t>(n);
            f.pos[0] = x0 + speed * 0.033f * static_cast<float>(n);
            f.pos[1] = y;
            for (int k : frozen)
                f.samePose = f.samePose || k == n;
            frames.push_back(f);
        }
        return frames;
    }

    void TestSamePose()
    {
        std::vector<replay::pose::BoneQts> a(40), b(40);
        for (std::size_t n = 0; n < a.size(); ++n)
        {
            a[n].translation[0] = b[n].translation[0] = 0.01f * static_cast<float>(n);
            a[n].rotation = b[n].rotation = AxisZ(0.02f * static_cast<float>(n));
        }
        Check(fp::SamePose(a.data(), b.data(), a.size()), "the same pose is the same");
        b[17].translation[2] += 0.0008f; // a re-sent held value: under the doubled codec epsilon
        b[3].rotation = AxisZ(0.06f + 0.003f);
        Check(fp::SamePose(a.data(), b.data(), a.size()), "a keyframe's rounding is still the same pose");
        b[17].translation[2] += 0.02f; // a foot 2 cm on: a walk
        Check(!fp::SamePose(a.data(), b.data(), a.size()), "one bone that moved is another pose");
        b = a;
        b[30].rotation = AxisZ(0.6f + 0.05f);
        Check(!fp::SamePose(a.data(), b.data(), a.size()), "one bone that turned is another pose");
        Check(!fp::SamePose(a.data(), b.data(), 0), "no bones: never the same");
    }

    void TestInView()
    {
        const fp::View cam = NorthCamera();
        const float ahead[3] = {0.0f, 10.0f, 0.0f};
        const float behind[3] = {0.0f, -10.0f, 0.0f};
        const float right[3] = {10.0f, 0.5f, 0.0f};
        // 50 degrees vertical at 16:9 is about 78 degrees wide: 39 each side.
        const float edgeIn[3] = {std::tan(0.65f) * 10.0f, 10.0f, 0.0f};  // 37 degrees right
        const float edgeOut[3] = {std::tan(0.72f) * 10.0f, 10.0f, 0.0f}; // 41 degrees right
        Check(fp::InView(cam, ahead), "straight ahead is in view");
        Check(!fp::InView(cam, behind), "behind the camera is off");
        Check(!fp::InView(cam, right), "beside the camera is off");
        Check(fp::InView(cam, edgeIn), "37 degrees off the axis is in a 16:9 view");
        Check(!fp::InView(cam, edgeOut), "41 degrees off the axis is out of it");
        fp::View west = cam;
        west.yawDeg = 90.0f; // counter-clockwise: heading 90 faces -X
        const float minusX[3] = {-10.0f, 0.0f, 0.0f};
        Check(fp::InView(west, minusX) && !fp::InView(west, ahead), "heading 90 faces -X");
        fp::View none;
        Check(!fp::InView(none, ahead), "no camera: nothing in view");
        Check(std::fabs(fp::GroundDistance(cam, right) - std::sqrt(100.25f)) < 1e-4f, "ground distance");
    }

    void TestCountPairs()
    {
        const auto cam = [](std::uint32_t) { return NorthCamera(); };
        // In view, 10 m ahead, walking 1.5 m/s: 30 frames, 29 pairs, 3 frozen.
        {
            fp::Counts c;
            fp::CountPairs(Walk(-1.0f, 10.0f, 1.5f, 30, {5, 6, 20}), cam, fp::kDefaultMaxDistM, c);
            Check(c.pairs[fp::kOn] == 29 && c.frozen[fp::kOn] == 3, "on camera: 29 pairs, 3 frozen");
            Check(c.pairs[fp::kOff] == 0, "nothing off camera");
        }
        // Behind the camera: the same frames count off camera.
        {
            fp::Counts c;
            fp::CountPairs(Walk(-1.0f, -10.0f, 1.5f, 30, {5, 6, 20}), cam, fp::kDefaultMaxDistM, c);
            Check(c.pairs[fp::kOff] == 29 && c.frozen[fp::kOff] == 3, "off camera: 29 pairs, 3 frozen");
            Check(std::fabs(fp::OffPercent(c) - 100.0f * 3.0f / 29.0f) < 0.01f, "off share");
        }
        // Standing still (or under 0.8 m/s) does not count: a still pose repeats by right.
        {
            fp::Counts c;
            fp::CountPairs(Walk(0.0f, -10.0f, 0.5f, 30, {1, 2, 3, 4}), cam, fp::kDefaultMaxDistM, c);
            Check(c.Pairs() == 0, "a slow ped is not judged");
        }
        // Farther than maxDistM is left out.
        {
            fp::Counts c;
            fp::CountPairs(Walk(0.0f, -60.0f, 1.5f, 30, {1}), cam, fp::kDefaultMaxDistM, c);
            Check(c.Pairs() == 0, "a ped 60 m away is not judged");
            fp::CountPairs(Walk(0.0f, -60.0f, 1.5f, 30, {1}), cam, 100.0f, c);
            Check(c.pairs[fp::kOff] == 29 && c.frozen[fp::kOff] == 1, "a wider radius takes it");
        }
        // A hole in the track (frames 100 ms apart) is not a pair.
        {
            std::vector<fp::Frame> frames = Walk(0.0f, -10.0f, 1.5f, 10, {});
            for (std::size_t n = 5; n < frames.size(); ++n)
                frames[n].timeMs += 100u;
            frames[5].samePose = true;
            fp::Counts c;
            fp::CountPairs(frames, cam, fp::kDefaultMaxDistM, c);
            Check(c.pairs[fp::kOff] == 8 && c.frozen[fp::kOff] == 0, "the pair across a hole is skipped");
        }
        // The camera turns away halfway: the split follows it per pair.
        {
            const auto turning = [](std::uint32_t t) {
                fp::View view = NorthCamera();
                view.yawDeg = t >= 1000u + 33u * 15u ? 180.0f : 0.0f;
                return view;
            };
            fp::Counts c;
            fp::CountPairs(Walk(-1.0f, 10.0f, 1.5f, 30, {3, 20, 21}), turning, fp::kDefaultMaxDistM, c);
            Check(c.pairs[fp::kOn] == 14 && c.frozen[fp::kOn] == 1, "on while the camera faces the ped");
            Check(c.pairs[fp::kOff] == 15 && c.frozen[fp::kOff] == 2, "off once it turned away");
        }
        // A carried body (wagon seat, saddle) moves with a still pose by right.
        {
            std::vector<fp::Frame> frames = Walk(0.0f, -10.0f, 1.5f, 30, {});
            for (std::size_t n = 0; n < frames.size(); ++n)
            {
                frames[n].samePose = n > 0;
                frames[n].carried = n < 20;
            }
            fp::Counts c;
            fp::CountPairs(frames, cam, fp::kDefaultMaxDistM, c);
            Check(c.pairs[fp::kOff] == 9 && c.frozen[fp::kOff] == 9, "only the pairs after the ped got down count");
        }
        // No recorded camera: nothing is judged.
        {
            fp::Counts c;
            fp::CountPairs(Walk(0.0f, 10.0f, 1.5f, 30, {1}), [](std::uint32_t) { return fp::View{}; }, 40.0f, c);
            Check(c.Pairs() == 0, "no camera, no pair");
        }
    }

    void TestVerdict()
    {
        fp::Counts ok;
        ok.pairs[fp::kOff] = 1000;
        ok.frozen[fp::kOff] = 4; // 0.4%: the builds before 1.0
        ok.pairs[fp::kOn] = 500;
        ok.frozen[fp::kOn] = 50; // the on camera share does not decide
        Check(!fp::Fails(ok, fp::kDefaultMaxPct), "0.4% off camera passes");
        fp::Counts bad = ok;
        bad.frozen[fp::kOff] = 180; // 18%: 1.0
        Check(fp::Fails(bad, fp::kDefaultMaxPct), "18% off camera fails");
        fp::Counts edge = ok;
        edge.frozen[fp::kOff] = 20; // exactly 2%
        Check(!fp::Fails(edge, 2.0f) && fp::Fails(edge, 1.9f), "the threshold is 'over', not 'at'");
        fp::Counts empty;
        Check(!fp::Fails(empty, fp::kDefaultMaxPct) && fp::OffPercent(empty) == 0.0f, "no pair passes");
        fp::Counts sum = ok;
        sum.Add(bad);
        Check(sum.pairs[fp::kOff] == 2000 && sum.frozen[fp::kOff] == 184, "per ped counts add up");
    }
}

int RunFrozenPoseTests()
{
    TestSamePose();
    TestInView();
    TestCountPairs();
    TestVerdict();
    if (g_frozenPoseFailures == 0)
        std::puts("frozen pose tests passed.");
    return g_frozenPoseFailures;
}
