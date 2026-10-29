// Tests for the pure part of the pose fidelity judge (pose_fidelity.h): key
// bone choice, histogram percentiles, error math, playback placement, storage
// (schedule, memory cap, buffer window), and an end-to-end synthetic check
// through the judge.
#include "pose_fidelity.h"
#include "clip_file.h"

#include <Windows.h> // GetEnvironmentVariableW (REPLAYPOC_POSE_CLIP_DIR)
#ifdef small
#undef small // rpcndr.h's macro: a name of this file (TestPercentiles)
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <thread>
#include <random>
#include <string>
#include <vector>

namespace
{
    using namespace replay::pose;
    using namespace replay::pose::fidelity;

    int g_fidelityFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_fidelityFailures;
    }

    BoneQts At(float x, float y, float z)
    {
        BoneQts bone;
        bone.translation[0] = x;
        bone.translation[1] = y;
        bone.translation[2] = z;
        return bone;
    }

    Quat AxisAngle(float x, float y, float z, float angle)
    {
        const float length = std::sqrt(x * x + y * y + z * z);
        const float s = std::sin(angle * 0.5f) / length;
        return Normalize({x * s, y * s, z * s, std::cos(angle * 0.5f)});
    }

    // ------------------------------------------------------------------
    // Key bones: an independent brute force of the same rule (the minimum
    // distance to the chosen bones recomputed from scratch for every bone).
    bool ReferenceUsable(const BoneQts& bone)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const float value = bone.translation[axis];
            if (!std::isfinite(value) || std::fabs(value) > 10000.0f)
                return false;
        }
        return true;
    }

    float ReferenceSquared(const BoneQts& a, const BoneQts& b)
    {
        const float dx = a.translation[0] - b.translation[0];
        const float dy = a.translation[1] - b.translation[1];
        const float dz = a.translation[2] - b.translation[2];
        return dx * dx + dy * dy + dz * dz;
    }

    std::vector<std::uint32_t> ReferenceKeyBones(const std::vector<BoneQts>& bones, std::uint32_t count)
    {
        std::vector<std::uint32_t> chosen;
        const std::uint32_t n = static_cast<std::uint32_t>(bones.size());
        for (std::uint32_t bone = 0; bone < n && chosen.empty() && count > 0; ++bone)
        {
            if (ReferenceUsable(bones[bone]))
                chosen.push_back(bone);
        }
        while (!chosen.empty() && chosen.size() < count)
        {
            std::uint32_t best = n;
            float bestDistance = -1.0f;
            for (std::uint32_t bone = 0; bone < n; ++bone)
            {
                if (!ReferenceUsable(bones[bone]) ||
                    std::find(chosen.begin(), chosen.end(), bone) != chosen.end())
                {
                    continue;
                }
                float nearest = (std::numeric_limits<float>::max)();
                for (const std::uint32_t other : chosen)
                    nearest = (std::min)(nearest, ReferenceSquared(bones[bone], bones[other]));
                if (nearest > bestDistance)
                {
                    bestDistance = nearest;
                    best = bone;
                }
            }
            if (best == n)
                break;
            chosen.push_back(best);
        }
        return chosen;
    }

    // FPS spreads the bones: the farthest any usable bone is from the
    // chosen ones is at most the smallest distance between two chosen ones.
    bool CoversWithinSeparation(const std::vector<BoneQts>& bones, const std::vector<std::uint32_t>& chosen)
    {
        float separation = (std::numeric_limits<float>::max)();
        for (std::size_t a = 0; a < chosen.size(); ++a)
        {
            for (std::size_t b = a + 1; b < chosen.size(); ++b)
                separation = (std::min)(separation, ReferenceSquared(bones[chosen[a]], bones[chosen[b]]));
        }
        float cover = 0.0f;
        for (const BoneQts& bone : bones)
        {
            if (!ReferenceUsable(bone))
                continue;
            float nearest = (std::numeric_limits<float>::max)();
            for (const std::uint32_t other : chosen)
                nearest = (std::min)(nearest, ReferenceSquared(bone, bones[other]));
            cover = (std::max)(cover, nearest);
        }
        return cover <= separation;
    }

    void TestKeyBones()
    {
        // A line of 101 bones: bone 0, the far end, the middle, the quarters.
        std::vector<BoneQts> line;
        for (int bone = 0; bone <= 100; ++bone)
            line.push_back(At(0.01f * static_cast<float>(bone), 0.0f, 0.0f));
        const std::vector<std::uint32_t> lineKeys = ChooseKeyBones(line.data(), 101);
        Check(lineKeys.size() == kKeyBones, "key bones: 16 of a line of 101");
        Check(lineKeys.size() >= 3 && lineKeys[0] == 0 && lineKeys[1] == 100 && lineKeys[2] == 50,
            "key bones: bone 0, then the far end, then the middle");
        Check(lineKeys == ReferenceKeyBones(line, kKeyBones), "key bones: line equals the brute force");

        // A metaped-sized cloud: the brute force, determinism, no duplicates,
        // the spread property.
        std::mt19937 random(11);
        std::uniform_real_distribution<float> coordinate(-1.0f, 1.0f);
        std::vector<BoneQts> cloud;
        for (int bone = 0; bone < 600; ++bone)
            cloud.push_back(At(0.4f * coordinate(random), 0.3f * coordinate(random), 0.9f + 0.9f * coordinate(random)));
        const std::vector<std::uint32_t> keys = ChooseKeyBones(cloud.data(), 600);
        Check(keys == ReferenceKeyBones(cloud, kKeyBones), "key bones: cloud equals the brute force");
        Check(keys == ChooseKeyBones(cloud.data(), 600), "key bones: deterministic");
        Check(keys.size() == kKeyBones && keys[0] == 0, "key bones: 16, from bone 0");
        std::vector<std::uint32_t> sorted = keys;
        std::sort(sorted.begin(), sorted.end());
        Check(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), "key bones: no bone twice");
        Check(CoversWithinSeparation(cloud, keys), "key bones: cover radius within the separation");

        // Unusable positions are never chosen, even the farthest ones.
        std::vector<BoneQts> broken = cloud;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        broken[5].translation[1] = nan;
        broken[77].translation[0] = inf;
        broken[200] = At(5.0e4f, 0.0f, 0.0f); // finite but absurd: would be the farthest
        broken[301].translation[2] = -inf;
        const std::vector<std::uint32_t> brokenKeys = ChooseKeyBones(broken.data(), 600);
        bool clean = brokenKeys.size() == kKeyBones;
        for (const std::uint32_t bone : brokenKeys)
            clean = clean && bone != 5 && bone != 77 && bone != 200 && bone != 301;
        Check(clean, "key bones: NaN, infinite and absurd positions are never chosen");
        Check(brokenKeys == ReferenceKeyBones(broken, kKeyBones), "key bones: broken cloud equals the brute force");
        broken[0].translation[0] = nan;
        const std::vector<std::uint32_t> noRoot = ChooseKeyBones(broken.data(), 600);
        Check(!noRoot.empty() && noRoot[0] == 1, "key bones: bone 0 unusable starts from the first usable bone");
        Check(noRoot == ReferenceKeyBones(broken, kKeyBones), "key bones: no root equals the brute force");

        // Fewer bones than keys: all of them; nothing usable: none.
        const std::vector<std::uint32_t> few = ChooseKeyBones(cloud.data(), 5);
        std::vector<std::uint32_t> fewSorted = few;
        std::sort(fewSorted.begin(), fewSorted.end());
        Check(fewSorted == std::vector<std::uint32_t>({0, 1, 2, 3, 4}), "key bones: 5 bones give all 5");
        std::vector<BoneQts> dead(4, At(nan, 0.0f, 0.0f));
        Check(ChooseKeyBones(dead.data(), 4).empty(), "key bones: nothing usable, nothing chosen");
        Check(ChooseKeyBones(nullptr, 4).empty() && ChooseKeyBones(cloud.data(), 0).empty() &&
                ChooseKeyBones(cloud.data(), 600, 0).empty(),
            "key bones: no input, no output");
        // Coincident bones still fill the count (ties by index).
        std::vector<BoneQts> same(20, At(0.1f, 0.2f, 0.3f));
        const std::vector<std::uint32_t> sameKeys = ChooseKeyBones(same.data(), 20);
        Check(sameKeys.size() == kKeyBones && sameKeys[0] == 0 && sameKeys[1] == 1 && sameKeys[15] == 15,
            "key bones: coincident bones by index");
    }

    // ------------------------------------------------------------------
    void TestPercentiles()
    {
        Check(NearestRank(0.95, 20) == 19 && NearestRank(0.5, 1) == 1 && NearestRank(0.99, 100) == 99 &&
                NearestRank(0.5, 3) == 2 && NearestRank(1.0, 7) == 7 && NearestRank(0.0, 7) == 1 &&
                NearestRank(0.5, 0) == 0 && NearestRank(0.95, 1000) == 950 && NearestRank(0.001, 10) == 1,
            "nearest rank");

        Distribution empty;
        Check(empty.Count() == 0 && empty.Percentile(0.95) == 0.0f && empty.Max() == 0.0f && empty.Mean() == 0.0,
            "empty distribution is zero");

        std::mt19937 random(5);
        std::exponential_distribution<float> exponential(1.0f / 20.0f);
        std::vector<float> values;
        for (int index = 0; index < 20000; ++index)
            values.push_back(exponential(random));
        for (int index = 0; index < 300; ++index)
            values.push_back(1500.0f + 4.0f * static_cast<float>(index)); // across 2 m into the overflow
        const float edges[] = {0.0f, 0.5f, 1.0f, 10.0f, 10.0001f, 50.0f, 200.0f, 1999.75f, 2000.0f, 2500.0f, 1.0e5f};
        values.insert(values.end(), std::begin(edges), std::end(edges));
        Distribution distribution;
        double sum = 0.0;
        std::uint64_t over10 = 0;
        std::uint64_t over50 = 0;
        std::uint64_t over200 = 0;
        for (const float value : values)
        {
            distribution.Add(value);
            sum += static_cast<double>(value);
            over10 += value > 10.0f ? 1u : 0u;
            over50 += value > 50.0f ? 1u : 0u;
            over200 += value > 200.0f ? 1u : 0u;
        }
        std::vector<float> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        Check(distribution.Count() == values.size() && distribution.Max() == sorted.back(), "count and exact max");
        Check(std::fabs(distribution.Mean() - sum / static_cast<double>(values.size())) <= 1e-9 * sum,
            "mean against the reference");
        Check(distribution.Over10() == over10 && distribution.Over50() == over50 && distribution.Over200() == over200,
            "counts over 10, 50 and 200");
        int compared = 0;
        int within = 0;
        const double ps[] = {0.001, 0.01, 0.25, 0.5, 0.9, 0.95, 0.98, 0.99, 0.995, 0.999, 1.0};
        for (const double p : ps)
        {
            const std::uint64_t rank = NearestRank(p, sorted.size());
            const float reference = sorted[static_cast<std::size_t>(rank - 1)];
            const float estimate = distribution.Percentile(p);
            ++compared;
            const bool ok = reference < 2000.0f
                ? (estimate >= reference && estimate <= reference + Distribution::kBinWidth &&
                      estimate <= distribution.Max())
                : estimate == distribution.Max();
            within += ok ? 1 : 0;
        }
        Check(within == compared, "percentiles: never below the sorted reference, at most one bin above");

        // Small sets, by hand: 0.2, 0.7, 1.2 -> p50 is 0.7, in the bin [0.5, 1.0).
        Distribution small;
        small.Add(0.2f);
        small.Add(0.7f);
        small.Add(1.2f);
        Check(small.Percentile(0.5) == 1.0f && small.Percentile(1.0) == 1.2f && small.Percentile(0.01) == 0.5f,
            "small set percentiles (the bin's upper edge, capped by the max)");

        // Not finite counts as kNonFiniteError; negative as 0; Reset clears.
        Distribution odd;
        odd.Add(std::numeric_limits<float>::quiet_NaN());
        odd.Add(-3.0f);
        odd.Add(std::numeric_limits<float>::infinity());
        Check(odd.Count() == 3 && odd.Max() == kNonFiniteError && odd.Over200() == 2 && odd.Percentile(0.01) == 0.5f,
            "not finite and negative values");
        odd.Reset();
        Check(odd.Count() == 0 && odd.Max() == 0.0f && odd.Over10() == 0 && odd.Percentile(0.5) == 0.0f, "reset");
        std::printf("  fidelity percentiles: %zu values, %d percentiles within one 0.5 bin of the sorted reference\n",
            values.size(), within);
    }

    // ------------------------------------------------------------------
    // A skeleton parent matrix as the engine stores it (DecodeWorldMatrix):
    // right, forward, up, position, the 32 m cell in the position's w lane.
    void MakeWorld(float heading, float x, float y, float z, std::int16_t cellX, std::int16_t cellY, float out[16])
    {
        std::memset(out, 0, sizeof(float) * 16);
        out[0] = std::cos(heading);
        out[1] = std::sin(heading);
        out[4] = -std::sin(heading);
        out[5] = std::cos(heading);
        out[10] = 1.0f;
        out[12] = x;
        out[13] = y;
        out[14] = z;
        const std::uint32_t cell = static_cast<std::uint16_t>(cellX) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(cellY)) << 16);
        std::memcpy(&out[15], &cell, sizeof(cell));
    }

    bool Near(double actual, double expected, double tolerance)
    {
        return std::fabs(actual - expected) <= tolerance;
    }

    void TestErrorMath()
    {
        BoneQts recorded = At(0.03f, 0.04f, 0.0f);
        Check(Near(PositionErrorMm({0.0f, 0.0f, 0.0f}, recorded), 50.0, 1e-3), "position error in mm");
        Check(PositionErrorMm({0.03f, 0.04f, 0.0f}, recorded) == 0.0f, "same position, zero error");
        recorded.rotation = AxisAngle(0.0f, 0.0f, 1.0f, 0.01f);
        Check(Near(RotationErrorMrad(AxisAngle(0.0f, 0.0f, 1.0f, 0.0f), recorded), 10.0, 0.01), "rotation error in mrad");
        const Quat q = AxisAngle(0.3f, -0.2f, 0.9f, 1.1f);
        recorded.rotation = {-q.x, -q.y, -q.z, -q.w};
        Check(RotationErrorMrad(q, recorded) < 0.01f, "rotation error is sign-agnostic");
        recorded.translation[1] = std::numeric_limits<float>::quiet_NaN();
        Check(PositionErrorMm({0.0f, 0.0f, 0.0f}, recorded) == kNonFiniteError, "NaN position counts as not finite");
        Check(PositionErrorMm({0.0f, 0.0f, 0.0f}, At(3.0e3f, 0.0f, 0.0f)) == kNonFiniteError,
            "an error over 1 km counts as not finite");

        // The playback placement: A blended with B by t, cells included.
        PoseSample sample;
        MakeWorld(0.2f, 1.0f, 2.0f, 3.0f, 2, -1, sample.worldA);
        MakeWorld(0.6f, 3.0f, 2.0f, 5.0f, 2, -1, sample.worldB);
        sample.t = 0.25f;
        WorldTransform placed{};
        WorldTransform a{};
        WorldTransform b{};
        Check(PlacementOf(sample, placed) && DecodeWorldMatrix(sample.worldA, a) && DecodeWorldMatrix(sample.worldB, b),
            "placement decodes");
        const WorldTransform blended = InterpolateWorld(a, b, 0.25f);
        Check(placed.position.x == blended.position.x && placed.position.y == blended.position.y &&
                placed.position.z == blended.position.z && Near(placed.position.x, 65.5, 1e-4) &&
                Near(placed.position.y, -30.0, 1e-4) && Near(placed.position.z, 3.5, 1e-4),
            "placement is InterpolateWorld of the decoded matrices (cell x 32 + local)");
        std::memset(sample.worldB, 0, sizeof(sample.worldB));
        Check(PlacementOf(sample, placed) && placed.position.x == a.position.x && placed.position.z == a.position.z,
            "placement without B is A");
        std::memset(sample.worldA, 0, sizeof(sample.worldA));
        Check(!PlacementOf(sample, placed), "no placement without A");

        // One comparison: key bones inside the sample, their errors and the origin.
        PoseSample recordedSample;
        recordedSample.objects.assign(5, BoneQts{});
        for (std::uint32_t bone = 0; bone < 5; ++bone)
        {
            recordedSample.objects[bone] = At(0.1f * static_cast<float>(bone), 0.0f, 1.0f);
            recordedSample.objects[bone].rotation = AxisAngle(1.0f, 0.0f, 0.0f, 0.1f * static_cast<float>(bone));
        }
        MakeWorld(0.0f, 10.0f, 4.0f, 1.0f, 0, 0, recordedSample.worldA);
        std::memcpy(recordedSample.worldB, recordedSample.worldA, sizeof(recordedSample.worldB));
        const std::uint32_t keys[3] = {3, 0, 9}; // 9: outside the sample's bones
        TruthSample truth;
        FillTruth(truth, keys, 2, recordedSample.objects.data());
        truth.position[0].y += 0.002f; // bone 3: 2 mm off
        truth.rotation[1] = AxisAngle(1.0f, 0.0f, 0.0f, 0.003f); // bone 0: 3 mrad off
        MakeWorld(0.0f, 10.0f, 4.006f, 1.0f, 0, 0, truth.world); // origin: 6 mm off
        SampleError error;
        Check(Compare(truth, keys, 3, recordedSample, error) && error.bones == 2 && error.bone[0] == 3 &&
                error.bone[1] == 0,
            "compare: the key bones inside the sample, in key order");
        Check(Near(error.positionMm[0], 2.0, 1e-3) && error.positionMm[1] == 0.0f &&
                Near(error.rotationMrad[1], 3.0, 0.02) && error.rotationMrad[0] < 0.01f,
            "compare: position and rotation errors per key bone");
        Check(error.hasOrigin && Near(error.originMm, 6.0, 2e-3), "compare: origin error");
        TruthSample exact;
        FillTruth(exact, keys, 2, recordedSample.objects.data());
        std::memcpy(exact.world, recordedSample.worldA, sizeof(exact.world));
        Check(Compare(exact, keys, 2, recordedSample, error) && error.positionMm[0] == 0.0f &&
                error.positionMm[1] == 0.0f && error.rotationMrad[0] < 0.01f && error.originMm == 0.0f,
            "compare: the sample's own pose is zero error");
        const std::uint32_t outside[1] = {7};
        Check(!Compare(exact, outside, 1, recordedSample, error) && error.bones == 0, "compare: no key bone inside");
        TruthSample notFinite = exact;
        notFinite.rotation[1].w = std::numeric_limits<float>::quiet_NaN();
        Check(TruthUsable(exact, 2) && !TruthUsable(notFinite, 2), "truth usable only when finite");
    }

    // ------------------------------------------------------------------
    void TestStore()
    {
        Check(IntervalMs(10) == 100 && IntervalMs(0) == 1000 && IntervalMs(1000) == 16 && IntervalMs(60) == 16,
            "interval from the rate, clamped to 1..60 Hz");
        std::vector<std::uint32_t> staggers;
        for (std::uint32_t id = 1; id <= 64; ++id)
        {
            const std::uint32_t stagger = StaggerMs(id * 0x10001u, 100);
            Check(stagger < 100, "stagger within the interval");
            staggers.push_back(stagger);
        }
        std::sort(staggers.begin(), staggers.end());
        Check(std::unique(staggers.begin(), staggers.end()) - staggers.begin() > 30, "staggers spread the records");

        // 60 fps ticks for 2 s at 10 Hz: 20 reads, one interval apart.
        Store store;
        const std::uint32_t id = 0x00230011u;
        std::vector<std::uint32_t> dueAt;
        for (std::uint32_t now = 1000; now < 3000; now += 16)
        {
            if (store.Due(id, now, 100))
                dueAt.push_back(now);
        }
        bool spaced = dueAt.size() >= 19 && dueAt.size() <= 20;
        for (std::size_t index = 1; index < dueAt.size(); ++index)
            spaced = spaced && dueAt[index] - dueAt[index - 1] >= 84 && dueAt[index] - dueAt[index - 1] <= 116;
        Check(spaced, "due 10 times a second, one interval apart");
        Check(!dueAt.empty() && dueAt[0] >= 1000 + StaggerMs(id, 100) && dueAt[0] < 1000 + StaggerMs(id, 100) + 16,
            "the first read waits for the stagger");
        // A long stall: one read, then the rate again (no burst).
        int afterStall = 0;
        for (std::uint32_t now = 9000; now < 9100; now += 16)
            afterStall += store.Due(id, now, 100) ? 1 : 0;
        Check(afterStall == 1, "after a stall the record reads once, not to catch up");

        // The memory cap: what does not fit is dropped and counted.
        Store capped;
        capped.SetCapBytes(sizeof(TruthSample) * 40);
        RecordSamples& record = capped.Record(7);
        TruthSample sample;
        int appended = 0;
        for (int index = 0; index < 100; ++index)
        {
            sample.timeMs = static_cast<std::uint32_t>(index) * 100u;
            appended += capped.Append(record, sample) ? 1 : 0;
        }
        Check(appended == 40 && record.dropped == 60 && record.Held() == 40 &&
                capped.Bytes() == sizeof(TruthSample) * 40,
            "memory cap: 40 kept, 60 dropped");
        Check(!capped.HasRoom(record) && !capped.HasRoom(capped.Record(8)), "memory cap: no room for any record");

        // Buffer mode: the last 120 s kept, records gone that long forgotten.
        Store buffer;
        buffer.SetBufferMode(true);
        RecordSamples& live = buffer.Record(1);
        for (std::uint32_t time = 0; time <= 200000; time += 100)
        {
            sample.timeMs = time;
            buffer.Append(live, sample);
        }
        buffer.Due(2, 1000, 100); // asked for once, long ago
        RecordSamples& idle = buffer.Record(2);
        sample.timeMs = 1000;
        buffer.Append(idle, sample);
        buffer.Due(3, 199000, 100); // active, nothing read yet
        buffer.Due(1, 199900, 100);
        const std::size_t bytesBefore = buffer.Bytes();
        buffer.Trim(200000);
        const RecordSamples* kept = buffer.Find(1);
        Check(kept && kept->Held() == 1201 && kept->samples[kept->head].timeMs == 80000 && buffer.HasRoom(*kept),
            "buffer keeps the last 120 s");
        Check(!buffer.Find(2) && buffer.Find(3) && buffer.Records().size() == 2 && buffer.Bytes() < bytesBefore,
            "buffer forgets records idle for the window");
        sample.timeMs = 200100;
        Check(buffer.Append(buffer.Record(1), sample) &&
                buffer.Find(1)->Held() == 1202 && buffer.Find(1)->samples.back().timeMs == 200100,
            "buffer appends after a trim");
        Store manual;
        RecordSamples& manualRecord = manual.Record(1);
        sample.timeMs = 0;
        manual.Append(manualRecord, sample);
        manual.Trim(500000);
        Check(manual.Find(1) && manual.Find(1)->Held() == 1, "manual recordings keep every read");
        buffer.Reset();
        Check(buffer.Records().empty() && buffer.Bytes() == 0 && !buffer.Find(1) && buffer.BufferMode(),
            "reset releases the reads, keeps the mode");
    }

    // ------------------------------------------------------------------
    // The buffer's trim runs on the clock. The store calls of one due read as
    // the mod makes them (pose_fidelity.cpp Sample): TrimOnClock, Record,
    // HasRoom (refused: dropped), Append; `afterKept` trims only after a kept
    // read, which deadlocks once the cap is full.
    bool BufferRead(Store& store, std::uint32_t id, std::uint32_t nowMs, bool afterKept, std::uint32_t& nextTrimMs)
    {
        if (!afterKept)
            store.TrimOnClock(nowMs);
        RecordSamples& record = store.Record(id);
        if (!store.HasRoom(record))
        {
            ++record.dropped;
            return false;
        }
        TruthSample sample;
        sample.timeMs = nowMs;
        if (!store.Append(record, sample))
            return false;
        if (afterKept && nowMs >= nextTrimMs)
        {
            store.Trim(nowMs);
            nextTrimMs = nowMs + kBufferTrimEveryMs;
        }
        return true;
    }

    void TestBufferTrimClock()
    {
        // 4 entities read at 20 Hz in 60 fps ticks for 400 s, a cap of 4000
        // reads: the cap is full after about a minute, before the first read
        // is 120 s old (like 64 entities at 20 Hz under the 64 MB cap).
        constexpr std::uint32_t kIds = 4;
        constexpr std::uint32_t kIntervalMs = 50;
        constexpr std::uint32_t kStartMs = 1000;
        constexpr std::uint32_t kEndMs = 400000;
        constexpr std::size_t kCapReads = 4000;
        Store stores[2]; // 0: the trim on the clock; 1: after a kept read
        std::uint32_t nextTrimMs[2] = {0, 0};
        std::uint32_t lastKeptMs[2] = {0, 0};
        std::uint64_t kept[2] = {0, 0};
        for (Store& store : stores)
        {
            store.SetBufferMode(true);
            store.SetCapBytes(sizeof(TruthSample) * kCapReads);
        }
        for (std::uint32_t now = kStartMs; now <= kEndMs; now += 16)
        {
            for (int mode = 0; mode < 2; ++mode)
            {
                for (std::uint32_t id = 1; id <= kIds; ++id)
                {
                    if (!stores[mode].Due(id, now, kIntervalMs))
                        continue;
                    if (BufferRead(stores[mode], id, now, mode == 1, nextTrimMs[mode]))
                    {
                        ++kept[mode];
                        lastKeptMs[mode] = now;
                    }
                }
            }
        }
        // On the clock: the reads aged out and new ones were kept again; the
        // store holds the last 120 s of what it kept, under the cap.
        const Store& onClock = stores[0];
        bool recent = onClock.Records().size() == kIds;
        bool window = recent;
        std::uint64_t dropped = 0;
        std::uint32_t newestOldest = kEndMs; // the oldest of the records' newest held reads
        for (const std::unique_ptr<RecordSamples>& record : onClock.Records())
        {
            dropped += record->dropped;
            recent = recent && record->Held() > 0;
            if (record->Held() == 0)
                continue;
            newestOldest = (std::min)(newestOldest, record->samples.back().timeMs);
            window = window && record->samples[record->head].timeMs + kBufferKeepMs + kBufferTrimEveryMs + 16 >= kEndMs;
        }
        recent = recent && newestOldest + 40000 >= kEndMs;
        Check(dropped > 0 && onClock.Bytes() <= sizeof(TruthSample) * kCapReads, "trim on the clock: the cap was hit");
        Check(recent && lastKeptMs[0] + 1000 >= kEndMs,
            "trim on the clock: reads are kept again after the first ones age out");
        Check(window, "trim on the clock: only the last 120 s are held");
        // After a kept read only: once the cap is full nothing is trimmed or
        // kept again, the first reads stay (the deadlock).
        const Store& stuck = stores[1];
        bool first = !stuck.Records().empty();
        for (const std::unique_ptr<RecordSamples>& record : stuck.Records())
            first = first && record->head == 0 && record->Held() > 0 && record->samples.front().timeMs < kStartMs + 100;
        Check(lastKeptMs[1] < kBufferKeepMs && first, "trim after a kept read: a full store keeps its first reads");
        std::printf("  fidelity buffer trim: on the clock %llu reads kept (last at %u ms, %llu dropped, newest held "
                    "of every record from %u ms on); after a kept read only %llu kept, the last at %u ms\n",
            static_cast<unsigned long long>(kept[0]), lastKeptMs[0], static_cast<unsigned long long>(dropped),
            newestOldest, static_cast<unsigned long long>(kept[1]), lastKeptMs[1]);
    }

    // ------------------------------------------------------------------
    // End to end: an 8-bone skeleton with a known motion (a root turning
    // fast about Z, a chain and a limb on it, an object-space bone), the
    // entity moving in a straight line while it turns. The track gets a
    // frame every 33 ms; the engine reads come from the same motion at the
    // frame times and between frames.
    constexpr std::uint32_t kBones = 8;
    constexpr float kRootTurn = 3.0f * 3.14159265f; // rad/s: 0.31 rad between frames
    constexpr std::uint32_t kFirstMs = 100;
    constexpr std::uint32_t kFrameMs = 33;
    constexpr std::uint32_t kFrames = 91; // 3 s: a relative keyframe chunk at 2 s

    SkeletonLayout SyntheticLayout()
    {
        SkeletonLayout layout;
        layout.boneCount = kBones;
        layout.order = CompositionOrder::ParentThenLocal;
        layout.inheritScale = false;
        layout.parent = {kRootParent, 0, 1, 2, 0, 4, kUnresolvedParent, 3};
        layout.ambiguous.assign(kBones, 0);
        layout.highPrecision.assign(kBones, 0);
        layout.highPrecision[0] = 1;
        for (std::uint32_t bone = 0; bone < kBones; ++bone)
            layout.evaluationOrder.push_back(bone);
        layout.unresolvedBones = {6};
        return layout;
    }

    void SyntheticPose(const SkeletonLayout& layout, float seconds, std::vector<BoneQts>& locals,
        std::vector<BoneQts>& objects)
    {
        locals.assign(kBones, BoneQts{});
        locals[0] = At(0.0f, 0.0f, 1.0f);
        locals[0].rotation = AxisAngle(0.0f, 0.0f, 1.0f, kRootTurn * seconds);
        locals[1] = At(1.0f, 0.0f, 0.0f);
        locals[1].rotation = AxisAngle(1.0f, 0.0f, 0.0f, 0.4f * std::sin(2.0f * seconds));
        locals[2] = At(0.0f, 0.5f, 0.0f);
        locals[2].rotation = AxisAngle(1.0f, 0.0f, 0.0f, 0.6f * std::sin(3.0f * seconds));
        locals[3] = At(0.0f, 0.0f, 0.4f);
        locals[4] = At(-0.3f, 0.0f, 0.2f);
        locals[4].rotation = AxisAngle(0.0f, 1.0f, 0.0f, 0.5f * std::sin(seconds));
        locals[5] = At(0.0f, 0.0f, -0.6f);
        locals[6] = At(0.2f + 0.1f * std::sin(2.0f * seconds), -0.4f, 1.5f); // object space
        locals[6].rotation = AxisAngle(0.0f, 0.0f, 1.0f, 0.7f * seconds);
        locals[7] = At(0.05f, 0.0f, 0.0f);
        objects.assign(kBones, BoneQts{});
        for (const std::uint32_t bone : layout.evaluationOrder)
        {
            const std::int32_t parent = layout.parent[bone];
            objects[bone] = parent >= 0
                ? Compose(objects[static_cast<std::uint32_t>(parent)], locals[bone], layout.order, layout.inheritScale)
                : locals[bone];
        }
    }

    void SyntheticWorld(float seconds, float out[16])
    {
        MakeWorld(0.3f * seconds, 5.0f + 3.0f * seconds, 7.0f + seconds, 0.5f, 3, -2, out);
    }

    // The quantization bound of a bone's object at a frame time: every
    // channel's decoded value is within the encoder's epsilon or its
    // quantization of the input (rotation: 0.002 rad, or the 32-bit
    // smallest-three code, three components half a step off and the
    // largest recomputed, up to ~0.0048 rad: 0.005 taken; translation:
    // 0.5 mm, the 16-bit step is finer: 0.6 mm taken); an ancestor's
    // rotation error moves the bone by at most that angle times the length
    // of the chain between them, and every translation error adds as is.
    struct Bound
    {
        double positionMm = 0.0;
        double rotationMrad = 0.0;
    };

    std::vector<Bound> QuantizationBounds(const SkeletonLayout& layout, const std::vector<BoneQts>& locals)
    {
        constexpr double kRotation = 0.005;
        constexpr double kTranslation = 0.0006;
        std::vector<Bound> bounds(kBones);
        for (std::uint32_t bone = 0; bone < kBones; ++bone)
        {
            double position = kTranslation;
            double rotation = kRotation;
            double chain = 0.0; // length from the bone up to the current ancestor
            std::uint32_t current = bone;
            while (layout.parent[current] >= 0)
            {
                const BoneQts& local = locals[current];
                chain += std::sqrt(static_cast<double>(local.translation[0]) * local.translation[0] +
                    static_cast<double>(local.translation[1]) * local.translation[1] +
                    static_cast<double>(local.translation[2]) * local.translation[2]);
                current = static_cast<std::uint32_t>(layout.parent[current]);
                position += kTranslation + kRotation * chain;
                rotation += kRotation;
            }
            bounds[bone] = {position * 1000.0 * 1.01 + 0.01, rotation * 1000.0 * 1.01 + 0.01};
        }
        return bounds;
    }

    class TrackOf final : public TrackSource
    {
    public:
        TrackOf(PoseTrack* track, std::uint32_t recordId) : m_track(track), m_recordId(recordId) {}
        bool Span(std::uint32_t recordId, std::uint32_t& firstMs, std::uint32_t& lastMs) override
        {
            if (recordId != m_recordId || m_track->Stats().frames == 0)
                return false;
            firstMs = m_track->Stats().firstTimeMs;
            lastMs = m_track->Stats().lastTimeMs;
            return true;
        }
        bool Sample(std::uint32_t recordId, std::uint32_t timeMs, PoseSample& sample) override
        {
            return recordId == m_recordId && m_track->Sample(timeMs, sample);
        }

    private:
        PoseTrack* m_track = nullptr;
        std::uint32_t m_recordId = 0;
    };

    void TestEndToEnd()
    {
        const SkeletonLayout layout = SyntheticLayout();
        StreamConfig config;
        PoseTrack track;
        track.Begin(layout, config);
        std::vector<BoneQts> locals;
        std::vector<BoneQts> objects;
        float world[16] = {};
        for (std::uint32_t frame = 0; frame < kFrames; ++frame)
        {
            const std::uint32_t timeMs = kFirstMs + frame * kFrameMs;
            SyntheticPose(layout, static_cast<float>(timeMs) / 1000.0f, locals, objects);
            SyntheticWorld(static_cast<float>(timeMs) / 1000.0f, world);
            FrameInput input;
            input.timeMs = timeMs;
            input.world = world;
            input.locals = locals.data();
            input.objects = objects.data();
            track.Append(input);
        }
        Check(track.Stats().frames == kFrames && track.Stats().keyframes >= 2, "synthetic track recorded");

        // The key bones from the first read: all 8 (fewer than 16).
        SyntheticPose(layout, static_cast<float>(kFirstMs) / 1000.0f, locals, objects);
        const std::vector<std::uint32_t> keys = ChooseKeyBones(objects.data(), kBones);
        const std::uint32_t keyCount = static_cast<std::uint32_t>(keys.size());
        Check(keyCount == kBones && keys[0] == 0, "synthetic key bones: every bone, from bone 0");
        const std::vector<Bound> bounds = QuantizationBounds(layout, locals);

        Store store;
        RecordSamples& record = store.Record(0x1234u);
        record.kind = Kind::Horse;
        record.boneCount = kBones;
        record.keyBones = keys;

        PoseSample sample;
        double frameMax = 0.0;
        double frameBoundMin = 1e9;
        double betweenMax = 0.0;
        double expectedMax = 0.0;
        double originMax = 0.0;
        double worstBetweenDeviation = 0.0;
        int frameReads = 0;
        int betweenReads = 0;
        int frameOk = 0;
        int betweenOk = 0;
        float directMax = 0.0f;
        for (std::uint32_t frame = 0; frame + 1 < kFrames; ++frame)
        {
            for (const std::uint32_t offset : {0u, 11u, 16u, 17u, 29u})
            {
                const std::uint32_t timeMs = kFirstMs + frame * kFrameMs + offset;
                const float seconds = static_cast<float>(timeMs) / 1000.0f;
                SyntheticPose(layout, seconds, locals, objects);
                TruthSample truth;
                truth.timeMs = timeMs;
                FillTruth(truth, keys.data(), keyCount, objects.data());
                SyntheticWorld(seconds, truth.world);
                store.Append(record, truth);
                SampleError error;
                if (!track.Sample(timeMs, sample) || !Compare(truth, keys.data(), keyCount, sample, error))
                {
                    Check(false, "synthetic read compared");
                    continue;
                }
                originMax = (std::max)(originMax, static_cast<double>(error.originMm));
                if (offset == 0)
                {
                    // At a frame time: the quantization bound.
                    ++frameReads;
                    bool ok = error.bones == kBones;
                    for (std::uint32_t index = 0; index < error.bones; ++index)
                    {
                        const Bound& bound = bounds[error.bone[index]];
                        ok = ok && error.positionMm[index] <= bound.positionMm &&
                            error.rotationMrad[index] <= bound.rotationMrad;
                        frameMax = (std::max)(frameMax, static_cast<double>(error.positionMm[index]));
                        frameBoundMin = (std::min)(frameBoundMin, bound.positionMm);
                        directMax = (std::max)(directMax, error.positionMm[index]);
                    }
                    frameOk += ok ? 1 : 0;
                    continue;
                }
                // Between frames: the interpolation error of this motion (the
                // exact frames blended as the sampler blends them, against the
                // pose at that instant), give or take the quantization bound.
                ++betweenReads;
                const std::uint32_t timeA = kFirstMs + frame * kFrameMs;
                const std::uint32_t timeB = timeA + kFrameMs;
                const float t = static_cast<float>(timeMs - timeA) / static_cast<float>(timeB - timeA);
                std::vector<BoneQts> localsA;
                std::vector<BoneQts> objectsA;
                std::vector<BoneQts> localsB;
                std::vector<BoneQts> objectsB;
                SyntheticPose(layout, static_cast<float>(timeA) / 1000.0f, localsA, objectsA);
                SyntheticPose(layout, static_cast<float>(timeB) / 1000.0f, localsB, objectsB);
                bool ok = error.bones == kBones;
                for (std::uint32_t index = 0; index < error.bones; ++index)
                {
                    const std::uint32_t bone = error.bone[index];
                    const BoneQts blended = Interpolate(objectsA[bone], objectsB[bone], t);
                    const double expected = PositionErrorMm(
                        {objects[bone].translation[0], objects[bone].translation[1], objects[bone].translation[2]},
                        blended);
                    const double expectedRotation = RotationErrorMrad(objects[bone].rotation, blended);
                    const double deviation = std::fabs(error.positionMm[index] - expected);
                    worstBetweenDeviation = (std::max)(worstBetweenDeviation, deviation / bounds[bone].positionMm);
                    ok = ok && deviation <= bounds[bone].positionMm &&
                        std::fabs(error.rotationMrad[index] - expectedRotation) <= bounds[bone].rotationMrad * 1.25;
                    betweenMax = (std::max)(betweenMax, static_cast<double>(error.positionMm[index]));
                    expectedMax = (std::max)(expectedMax, expected);
                    directMax = (std::max)(directMax, error.positionMm[index]);
                }
                betweenOk += ok ? 1 : 0;
            }
        }
        Check(frameReads > 0 && frameOk == frameReads, "at the frame times every key bone is within the quantization bound");
        Check(betweenReads > 0 && betweenOk == betweenReads,
            "between frames every key bone is the expected interpolation error, within the quantization bound");
        Check(expectedMax > 5.0 && betweenMax > frameMax, "the judge sees this motion's interpolation error between frames");
        Check(originMax < 0.05, "origin: a straight line blends exactly");

        // The whole evaluation over the stored reads, plus two outside the span.
        TruthSample early;
        early.timeMs = kFirstMs - 50;
        FillTruth(early, keys.data(), keyCount, objects.data());
        TruthSample late = early;
        late.timeMs = kFirstMs + (kFrames - 1) * kFrameMs + 50;
        store.Append(record, late); // out of time order on purpose: the evaluation sorts
        store.Append(record, early);
        record.skipped = 3;
        TrackOf source(&track, 0x1234u);
        Report report;
        EvaluateStore(store, source, report);
        const KindSummary& all = report.all;
        const std::uint64_t reads = static_cast<std::uint64_t>(frameReads + betweenReads);
        Check(all.records == 1 && all.samples == reads && all.bones == reads * kBones && all.outside == 2 &&
                all.skipped == 3 && all.dropped == 0,
            "evaluation: counts");
        Check(report.kinds[static_cast<std::size_t>(Kind::Horse)].samples == reads &&
                report.kinds[static_cast<std::size_t>(Kind::Ped)].records == 0,
            "evaluation: the record's kind");
        Check(std::fabs(all.position.Max() - directMax) <= 1e-4f, "evaluation: the max is the direct comparisons' max");
        Check(all.origin.Count() == reads && all.origin.Max() < 0.05f, "evaluation: origin");
        Check(report.records.size() == 1 && report.records[0].outside == 2 && report.records[0].samples == reads &&
                report.records[0].posMaxMm == all.position.Max() && report.records[0].worstBone == all.worstBone &&
                all.worstRecord == 0x1234u,
            "evaluation: the record's line");
        const std::string line = SummaryLine("all", all, 10, 1.24);
        const std::string recordLine = RecordLine(report.records[0]);
        Check(line.rfind("POSE_FIDELITY_SUMMARY,kind=all,records=1,samples=", 0) == 0 &&
                line.find(",outside=2,skipped=3,dropped=0,pos_mean_mm=") != std::string::npos &&
                line.find(",worst_record=0x1234,") != std::string::npos &&
                line.find(",hz=10,eval_ms=1.2") != std::string::npos,
            "summary line");
        Check(recordLine.rfind("POSE_FIDELITY,record=0x1234,kind=horse,samples=", 0) == 0 &&
                recordLine.find(",worst_bone=") != std::string::npos,
            "record line");
        std::printf("  fidelity synthetic: %d reads at frames (max %.2f mm, tightest bound %.2f mm), %d between "
                    "(max %.2f mm, expected interpolation up to %.2f mm, worst deviation %.0f%% of the bound), origin "
                    "max %.4f mm\n",
            frameReads, frameMax, frameBoundMin, betweenReads, betweenMax, expectedMax, 100.0 * worstBetweenDeviation,
            originMax);
        std::printf("  %s\n", line.c_str());
    }

    // ------------------------------------------------------------------
    // A read is stamped with the time the capture gives its content
    // (capture::ContentTimeMs), not with the time the script read it. The
    // engine buffer keeps a finalizer call's output until the next call, and
    // the script reads it up to a frame later. A motion the track reproduces
    // exactly between frames (constant velocities: the entity at 7.7 m/s and
    // an object-space bone sliding at 2 m/s): finalizer calls at 60 fps, every
    // other one recorded (the 30 Hz gate), a read 8 ms after each call.
    // Stamped at the read, the judge would see speed x 8 ms on an exact track;
    // stamped at the call, recorded or not, the quantization only.
    constexpr float kEntitySpeed = 7.7f; // m/s, the entity origin along x
    constexpr float kSlideSpeed = 2.0f;  // m/s, bone 6 (object space) along y
    constexpr std::uint32_t kReadDelayMs = 8;

    void SlidingPose(const SkeletonLayout& layout, float seconds, std::vector<BoneQts>& locals,
        std::vector<BoneQts>& objects)
    {
        locals.assign(kBones, BoneQts{});
        locals[0] = At(0.0f, 0.0f, 1.0f);
        locals[1] = At(1.0f, 0.0f, 0.0f);
        locals[2] = At(0.0f, 0.5f, 0.0f);
        locals[3] = At(0.0f, 0.0f, 0.4f);
        locals[4] = At(-0.3f, 0.0f, 0.2f);
        locals[5] = At(0.0f, 0.0f, -0.6f);
        locals[6] = At(0.2f, -2.5f + kSlideSpeed * seconds, 1.5f); // within the 4 m s16 range
        locals[7] = At(0.05f, 0.0f, 0.0f);
        objects.assign(kBones, BoneQts{});
        for (const std::uint32_t bone : layout.evaluationOrder)
        {
            const std::int32_t parent = layout.parent[bone];
            objects[bone] = parent >= 0
                ? Compose(objects[static_cast<std::uint32_t>(parent)], locals[bone], layout.order, layout.inheritScale)
                : locals[bone];
        }
    }

    void TestContentTime()
    {
        const SkeletonLayout layout = SyntheticLayout();
        StreamConfig config;
        PoseTrack track;
        track.Begin(layout, config);
        std::vector<BoneQts> locals;
        std::vector<BoneQts> objects;
        constexpr std::uint32_t kCalls = 181; // 3 s at 60 fps
        auto callMs = [](std::uint32_t call) { return kFirstMs + call * 50u / 3u; };
        for (std::uint32_t call = 0; call < kCalls; call += 2) // the gate records every other call
        {
            const std::uint32_t timeMs = callMs(call);
            const float seconds = static_cast<float>(timeMs) / 1000.0f;
            SlidingPose(layout, seconds, locals, objects);
            float world[16] = {};
            MakeWorld(0.0f, 5.0f + kEntitySpeed * seconds, 7.0f, 0.5f, 3, -2, world);
            FrameInput input;
            input.timeMs = timeMs;
            input.world = world;
            input.locals = locals.data();
            input.objects = objects.data();
            track.Append(input);
        }
        Check(track.Stats().frames == (kCalls + 1) / 2, "content time: the 30 Hz track");
        SlidingPose(layout, static_cast<float>(kFirstMs) / 1000.0f, locals, objects);
        const std::vector<std::uint32_t> keys = ChooseKeyBones(objects.data(), kBones);
        const std::uint32_t keyCount = static_cast<std::uint32_t>(keys.size());
        const std::vector<Bound> bounds = QuantizationBounds(layout, locals);

        Store atRead;
        Store atContent;
        for (Store* store : {&atRead, &atContent})
        {
            RecordSamples& record = store->Record(0x5678u);
            record.kind = Kind::Horse;
            record.boneCount = kBones;
            record.keyBones = keys;
        }
        PoseSample sample;
        int reads = 0;
        int contentOk = 0;
        int readOk = 0;
        double contentPositionMax = 0.0;
        double contentOriginMax = 0.0;
        for (std::uint32_t call = 0; call + 1 < kCalls; ++call)
        {
            // What the buffer holds from this call until the next one.
            const std::uint32_t contentMs = callMs(call);
            const float seconds = static_cast<float>(contentMs) / 1000.0f;
            SlidingPose(layout, seconds, locals, objects);
            TruthSample truth;
            FillTruth(truth, keys.data(), keyCount, objects.data());
            MakeWorld(0.0f, 5.0f + kEntitySpeed * seconds, 7.0f, 0.5f, 3, -2, truth.world);
            ++reads;
            SampleError error;
            truth.timeMs = contentMs;
            atContent.Append(atContent.Record(0x5678u), truth);
            bool ok = track.Sample(contentMs, sample) && Compare(truth, keys.data(), keyCount, sample, error) &&
                error.bones == kBones && error.hasOrigin && error.originMm < 0.05f;
            for (std::uint32_t index = 0; ok && index < error.bones; ++index)
            {
                const Bound& bound = bounds[error.bone[index]];
                ok = error.positionMm[index] <= bound.positionMm && error.rotationMrad[index] <= bound.rotationMrad;
                contentPositionMax = (std::max)(contentPositionMax, static_cast<double>(error.positionMm[index]));
            }
            contentOriginMax = (std::max)(contentOriginMax, static_cast<double>(error.originMm));
            contentOk += ok ? 1 : 0;
            // The same content stamped at the read: the track has moved on.
            truth.timeMs = contentMs + kReadDelayMs;
            atRead.Append(atRead.Record(0x5678u), truth);
            ok = track.Sample(truth.timeMs, sample) && Compare(truth, keys.data(), keyCount, sample, error) &&
                error.hasOrigin && Near(error.originMm, kEntitySpeed * static_cast<double>(kReadDelayMs), 0.1);
            for (std::uint32_t index = 0; ok && index < error.bones; ++index)
            {
                if (error.bone[index] == 6)
                    ok = Near(error.positionMm[index], kSlideSpeed * static_cast<double>(kReadDelayMs),
                        bounds[6].positionMm);
            }
            readOk += ok ? 1 : 0;
        }
        Check(contentOk == reads,
            "content time: every read, at a recorded call or between two, within the quantization bound");
        Check(readOk == reads, "content time: stamped at the read, speed x delay on the exact track");
        TrackOf source(&track, 0x5678u);
        Report byContent;
        Report byRead;
        EvaluateStore(atContent, source, byContent);
        EvaluateStore(atRead, source, byRead);
        Check(byContent.all.samples == static_cast<std::uint64_t>(reads) && byContent.all.outside == 0 &&
                byRead.all.samples == static_cast<std::uint64_t>(reads) && byRead.all.outside == 0,
            "content time: every read inside the track");
        Check(byContent.all.origin.Max() < 0.05f &&
                Near(byContent.all.position.Max(), contentPositionMax, 1e-4) &&
                Near(byRead.all.origin.Mean(), kEntitySpeed * static_cast<double>(kReadDelayMs), 0.1) &&
                byRead.all.position.Max() > byContent.all.position.Max() + 10.0f,
            "content time: the evaluation sees the stamp");
        std::printf("  fidelity content time: %d reads 8 ms after the call; stamped at the call origin max %.4f mm, "
                    "position max %.2f mm; at the read origin mean %.2f mm, position max %.2f mm\n",
            reads, contentOriginMax, contentPositionMax, byRead.all.origin.Mean(),
            static_cast<double>(byRead.all.position.Max()));
    }

    // ------------------------------------------------------------------
    // The evaluation on a worker over ExportedTracks (the capture's export
    // imported into tracks of its own) writes exactly the text the stop tick
    // writes over the capture's tracks (capture::TrackSpan / SampleTrack,
    // TrackOf here): the same reports field by field and the same lines byte
    // for byte, at the same eval_ms.
    class TracksOf final : public TrackSource
    {
    public:
        void Add(std::uint32_t recordId, PoseTrack* track) { m_tracks.emplace(recordId, track); }
        bool Span(std::uint32_t recordId, std::uint32_t& firstMs, std::uint32_t& lastMs) override
        {
            const auto found = m_tracks.find(recordId);
            if (found == m_tracks.end() || found->second->Stats().frames == 0)
                return false;
            firstMs = found->second->Stats().firstTimeMs;
            lastMs = found->second->Stats().lastTimeMs;
            return true;
        }
        bool Sample(std::uint32_t recordId, std::uint32_t timeMs, PoseSample& sample) override
        {
            const auto found = m_tracks.find(recordId);
            return found != m_tracks.end() && found->second->Sample(timeMs, sample);
        }

    private:
        std::map<std::uint32_t, PoseTrack*> m_tracks;
    };

    bool SameDistribution(const Distribution& a, const Distribution& b)
    {
        if (a.Count() != b.Count() || a.Mean() != b.Mean() || a.Max() != b.Max() || a.Over10() != b.Over10() ||
            a.Over50() != b.Over50() || a.Over200() != b.Over200())
            return false;
        for (const double p : {0.5, 0.9, 0.95, 0.99, 1.0})
        {
            if (a.Percentile(p) != b.Percentile(p))
                return false;
        }
        return true;
    }

    bool SameSummary(const KindSummary& a, const KindSummary& b)
    {
        return a.records == b.records && a.samples == b.samples && a.bones == b.bones && a.outside == b.outside &&
            a.skipped == b.skipped && a.dropped == b.dropped && SameDistribution(a.position, b.position) &&
            SameDistribution(a.rotation, b.rotation) && SameDistribution(a.origin, b.origin) &&
            a.worstMm == b.worstMm && a.worstRecord == b.worstRecord && a.worstMs == b.worstMs &&
            a.worstBone == b.worstBone;
    }

    bool SameReport(const Report& a, const Report& b)
    {
        if (!SameSummary(a.all, b.all) || a.records.size() != b.records.size())
            return false;
        for (std::size_t kind = 0; kind < kKindCount; ++kind)
        {
            if (!SameSummary(a.kinds[kind], b.kinds[kind]))
                return false;
        }
        for (std::size_t index = 0; index < a.records.size(); ++index)
        {
            const RecordResult& x = a.records[index];
            const RecordResult& y = b.records[index];
            if (x.recordId != y.recordId || x.kind != y.kind || x.samples != y.samples || x.bones != y.bones ||
                x.outside != y.outside || x.skipped != y.skipped || x.dropped != y.dropped ||
                x.posMeanMm != y.posMeanMm || x.posP95Mm != y.posP95Mm || x.posMaxMm != y.posMaxMm ||
                x.rotP95Mrad != y.rotP95Mrad || x.originMaxMm != y.originMaxMm || x.worstMs != y.worstMs ||
                x.worstBone != y.worstBone)
                return false;
        }
        return true;
    }

    // Reads of one record taken from a track at `offsetMs` after each read
    // time (so the errors are not zero), every `stepMs` over the span and
    // two outside it.
    void ReadsFromTrack(Store& store, std::uint32_t recordId, Kind kind, PoseTrack& truthTrack, std::uint32_t stepMs,
        std::uint32_t offsetMs)
    {
        const std::uint32_t firstMs = truthTrack.Stats().firstTimeMs;
        const std::uint32_t lastMs = truthTrack.Stats().lastTimeMs;
        PoseSample sample;
        if (!truthTrack.Sample(firstMs, sample) || sample.objects.empty())
            return;
        RecordSamples& record = store.Record(recordId);
        record.kind = kind;
        record.boneCount = static_cast<std::uint32_t>(sample.objects.size());
        record.keyBones = ChooseKeyBones(sample.objects.data(), record.boneCount);
        const std::uint32_t keyCount = static_cast<std::uint32_t>(record.keyBones.size());
        std::vector<std::uint32_t> times;
        for (std::uint32_t timeMs = firstMs; timeMs <= lastMs; timeMs += stepMs)
            times.push_back(timeMs);
        times.push_back(lastMs + 40); // outside the span
        if (firstMs > 40)
            times.push_back(firstMs - 40);
        for (const std::uint32_t timeMs : times)
        {
            const std::uint32_t truthMs = (std::min)(timeMs + offsetMs, lastMs);
            if (!truthTrack.Sample((std::max)(firstMs, truthMs), sample))
                continue;
            TruthSample truth;
            truth.timeMs = timeMs;
            std::memcpy(truth.world, sample.worldA, sizeof(truth.world));
            FillTruth(truth, record.keyBones.data(), keyCount, sample.objects.data());
            store.Append(record, truth);
        }
    }

    void TestExportedTracks()
    {
        // Synthetic: three tracks recorded the way the capture records them,
        // one with a layout change (a new chunk on another layout), one whose
        // leading chunks the buffer dropped (DropBefore); sealed as StopAll
        // leaves them. Reads for them and for an id without a track.
        const SkeletonLayout layout = SyntheticLayout();
        SkeletonLayout other = layout;
        other.parent[6] = 0; // bone 6 resolved: another layout
        other.unresolvedBones.clear();
        StreamConfig config;
        PoseTrack tracks[3];
        std::vector<BoneQts> locals;
        std::vector<BoneQts> objects;
        float world[16] = {};
        for (int index = 0; index < 3; ++index)
        {
            tracks[index].Begin(layout, config);
            for (std::uint32_t frame = 0; frame < kFrames * 3; ++frame)
            {
                const std::uint32_t timeMs = kFirstMs + frame * kFrameMs;
                if (index == 1 && frame == kFrames)
                    tracks[index].UpdateLayout(other);
                const float seconds = static_cast<float>(timeMs) / 1000.0f + 0.37f * static_cast<float>(index);
                SyntheticPose(index == 1 && frame >= kFrames ? other : layout, seconds, locals, objects);
                SyntheticWorld(seconds, world);
                FrameInput input;
                input.timeMs = timeMs;
                input.world = world;
                input.locals = locals.data();
                input.objects = objects.data();
                tracks[index].Append(input);
            }
        }
        const std::size_t droppedChunks = tracks[2].DropBefore(kFirstMs + kFrames * kFrameMs);
        Store store;
        TracksOf direct;
        const std::uint32_t ids[3] = {0x100u, 0x200u, 0x300u};
        const Kind kinds[3] = {Kind::Ped, Kind::Horse, Kind::Vehicle};
        std::vector<PoseRecordExport> exports;
        for (int index = 0; index < 3; ++index)
        {
            tracks[index].Seal();
            PoseRecordExport exported;
            exported.recordId = ids[index];
            exported.boneCount = kBones;
            tracks[index].Export(exported.track);
            exports.push_back(std::move(exported));
            ReadsFromTrack(store, ids[index], kinds[index], tracks[index], 37, 11);
            direct.Add(ids[index], &tracks[index]);
        }
        {
            // A record the capture has no track for: every read outside.
            RecordSamples& record = store.Record(0x400u);
            record.kind = Kind::Other;
            record.boneCount = kBones;
            record.keyBones = store.Find(0x100u)->keyBones;
            TruthSample truth = store.Find(0x100u)->samples.front();
            store.Append(record, truth);
            record.skipped = 2;
        }
        // The worker's copy of the exports, taken before the direct pass
        // warms the original tracks' caches (as the playback that may start
        // right after the stop would).
        std::vector<PoseRecordExport> workerExports = exports;
        Report expected;
        EvaluateStore(store, direct, expected);
        // The store moved as Evaluate moves it, the evaluation on a thread
        // while this thread samples the original tracks (a playback), and
        // this thread's decode counter only counts its own decodes.
        auto moved = std::make_shared<Store>(std::move(store));
        Report fromWorker;
        std::uint64_t workerImportFailures = 0;
        std::uint64_t workerDecodes = 0;
        std::thread worker([&]() {
            const std::uint64_t before = DecodedFrameCount();
            ExportedTracks copies(std::move(workerExports));
            EvaluateStore(*moved, copies, fromWorker);
            workerImportFailures = copies.ImportFailures();
            workerDecodes = DecodedFrameCount() - before;
        });
        const std::uint64_t mainBefore = DecodedFrameCount();
        PoseSample sample;
        std::uint64_t playbackSamples = 0;
        for (int pass = 0; pass < 20; ++pass)
        {
            for (int index = 0; index < 3; ++index)
            {
                for (std::uint32_t timeMs = tracks[index].Stats().firstTimeMs; timeMs <= tracks[index].Stats().lastTimeMs;
                     timeMs += 16)
                    playbackSamples += tracks[index].Sample(timeMs, sample) ? 1 : 0;
            }
        }
        const std::uint64_t mainDecodes = DecodedFrameCount() - mainBefore;
        worker.join();
        const std::string expectedText = ReportText(expected, 10, 12.3, true);
        const std::string workerText = ReportText(fromWorker, 10, 12.3, true);
        Check(droppedChunks > 0, "exported tracks: the buffer track lost its leading chunks");
        Check(expected.all.samples > 0 && expected.all.outside >= 4 && expected.records.size() == 4,
            "exported tracks: synthetic reads compared, some outside");
        Check(workerImportFailures == 0, "exported tracks: every export imports");
        Check(SameReport(expected, fromWorker), "exported tracks: the worker's report is the stop tick's, field by field");
        Check(expectedText == workerText, "exported tracks: the same lines byte for byte");
        Check(workerDecodes > 0 && mainDecodes > 0 && playbackSamples > 0,
            "exported tracks: both threads decode, each counter its own thread's");
        // ReportText is the stop's lines: per record (Debug), then all and
        // the kinds with records.
        std::string lines;
        for (const RecordResult& record : expected.records)
            lines += RecordLine(record) + "\n";
        lines += SummaryLine("all", expected.all, 10, 12.3) + "\n";
        for (std::size_t kind = 0; kind < kKindCount; ++kind)
        {
            if (expected.kinds[kind].records != 0)
                lines += SummaryLine(KindText(static_cast<Kind>(kind)), expected.kinds[kind], 10, 12.3) + "\n";
        }
        Check(lines == expectedText, "report text: the lines Evaluate wrote, in its order");
        Check(ReportText(expected, 10, 12.3, false) == lines.substr(lines.find("POSE_FIDELITY_SUMMARY")),
            "report text: summaries only without Debug");
        std::printf("  fidelity exported tracks (synthetic): %llu reads compared, %llu outside, %zu records, "
                    "worker decodes %llu, playback thread decodes %llu, same text %d\n",
            static_cast<unsigned long long>(expected.all.samples), static_cast<unsigned long long>(expected.all.outside),
            expected.records.size(), static_cast<unsigned long long>(workerDecodes),
            static_cast<unsigned long long>(mainDecodes), expectedText == workerText ? 1 : 0);

        // Clips (REPLAYPOC_POSE_CLIP_DIR): every pose record of each clip as
        // the capture's track (imported from the file), reads taken from it
        // 7 ms off every 100 ms; the worker evaluates the export of those
        // tracks, re-imported.
        wchar_t folder[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"REPLAYPOC_POSE_CLIP_DIR", folder, MAX_PATH))
            return;
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
        {
            if (entry.path().extension() != L".rpclip")
                continue;
            replay::Recording recording;
            std::vector<PoseRecordExport> poses;
            replay::camera::Track camera;
            std::uint64_t duration = 0;
            std::string error;
            if (!replay::clipfile::Load(entry.path().wstring(), 0x140000000ull, 0x8000000, recording, poses, camera,
                    duration, error))
                continue;
            std::vector<std::unique_ptr<PoseTrack>> captured;
            TracksOf capture;
            Store clipStore;
            std::vector<PoseRecordExport> clipExports;
            for (const PoseRecordExport& record : poses)
            {
                auto track = std::make_unique<PoseTrack>();
                std::string importError;
                if (record.track.chunks.empty() || !track->Import(record.track, importError))
                    continue;
                PoseRecordExport exported;
                exported.recordId = record.recordId;
                exported.boneCount = record.boneCount;
                track->Export(exported.track);
                clipExports.push_back(std::move(exported));
                ReadsFromTrack(clipStore, record.recordId, static_cast<Kind>(record.recordId % kKindCount), *track, 100, 7);
                capture.Add(record.recordId, track.get());
                captured.push_back(std::move(track));
            }
            Report clipExpected;
            EvaluateStore(clipStore, capture, clipExpected);
            Report clipWorker;
            std::uint64_t failures = 0;
            std::thread clipThread([&]() {
                ExportedTracks copies(std::move(clipExports));
                EvaluateStore(clipStore, copies, clipWorker);
                failures = copies.ImportFailures();
            });
            clipThread.join();
            const bool same = SameReport(clipExpected, clipWorker) &&
                ReportText(clipExpected, 10, 1.0, true) == ReportText(clipWorker, 10, 1.0, true);
            Check(failures == 0 && same, "exported tracks: a clip's evaluation on the worker is the stop tick's");
            std::printf("  fidelity exported tracks %ls: %zu records, %llu reads compared, %llu outside, "
                        "pos p95 %.2f mm, max %.2f mm, same %d\n",
                entry.path().filename().c_str(), clipExpected.records.size(),
                static_cast<unsigned long long>(clipExpected.all.samples),
                static_cast<unsigned long long>(clipExpected.all.outside),
                static_cast<double>(clipExpected.all.position.Percentile(0.95)),
                static_cast<double>(clipExpected.all.position.Max()), same ? 1 : 0);
        }
    }
}

int RunPoseFidelityTests()
{
    TestKeyBones();
    TestPercentiles();
    TestErrorMath();
    TestStore();
    TestBufferTrimClock();
    TestEndToEnd();
    TestContentTime();
    TestExportedTracks();
    if (g_fidelityFailures == 0)
        std::printf("Pose fidelity tests passed.\n");
    return g_fidelityFailures;
}
