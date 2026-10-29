// The pure rules of src/pose_capture_rules.h against the behaviour they keep
// or extend.
//
// End-of-frame check: the copy check (copy the engine buffer,
// CompareLatePoseChanged, one check in kLateVerifyEvery through
// CompareLatePose) is kept here as the reference, and the in-place check
// (compare the buffer in place first, settle equal bytes without the copy)
// must give the same change, bit for bit, and the same counters, on
// synthetic skeletons and on the recorded clips (REPLAYPOC_POSE_CLIP_DIR),
// with the shortcuts on and off.
// Stall polls: StallPollDue's table, and the per-tick poll decision over
// simulated finalizer call patterns: with the key at 0 it is the plain
// per-tick rule tick for tick; with N it polls only stalled records, and
// every tick the plain rule would have polled is a stall poll or a skipped
// one; at the default (250 ms) an off camera NPC is never polled, a skeleton
// the finalizer never visits on every tick.
// Late backoff: NextLateCheckMs without the backoff is the fixed wait for
// every input; with it, the 1, 2, 4, 8 s waits, the 250 ms on a change and
// the restart at 1 s; the first check's stagger spreads the records of a
// scan.
#include "capture_capacity.h"
#include "entity_selection.h"
#include "object_pose.h"
#include "pose_capture_rules.h"
#include "pose_skeleton_layout.h"
#include "pose_stream.h"
#include "recording_window.h"
#include "replay_buffer.h"
#ifdef _WIN32
#include "clip_file.h"

#include <Windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace
{
    using namespace replay::pose;
    using namespace replay::pose::capture;

    int g_rulesFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_rulesFailures;
    }

    double NowUs()
    {
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    Quat AxisAngle(float x, float y, float z, float angle)
    {
        const float length = std::sqrt(x * x + y * y + z * z);
        const float s = std::sin(angle * 0.5f) / length;
        return Normalize({x * s, y * s, z * s, std::cos(angle * 0.5f)});
    }

    // pose_capture.cpp's tolerances (kLateTranslation, kLateRotation).
    constexpr float kTranslation = 0.01f;
    constexpr float kRotation = 0.01f;

    // ---------------------------------------------------------------------
    // End-of-frame check.
    struct LateCounters
    {
        std::uint64_t checks = 0;
        std::uint64_t equal = 0;
        std::uint64_t verified = 0;
        std::uint64_t mismatch = 0;
        std::uint64_t direct = 0;
    };

    struct LateWork
    {
        std::vector<BoneQts> late; // the copy of the engine buffer
        std::vector<BoneQts> scratch;
        std::vector<std::uint8_t> mask;
    };

    bool SameChange(const LatePoseChange& a, const LatePoseChange& b)
    {
        return a.localBones == b.localBones && a.objectBones == b.objectBones && a.worstBone == b.worstBone &&
            std::memcmp(&a.maxTranslation, &b.maxTranslation, sizeof(float)) == 0 &&
            std::memcmp(&a.maxRotation, &b.maxRotation, sizeof(float)) == 0;
    }

    // The reference: the comparison after the copy (the check's body from
    // `++record.lateChecks` to the change), with its counters.
    LatePoseChange ReferenceLateCompare(bool shortcuts, LateCounters& counters, const BoneQts* finalizer,
        const BoneQts* engine, const std::uint16_t* parents, std::uint32_t boneCount, float rootScale, LateWork& work)
    {
        work.late.assign(engine, engine + static_cast<std::size_t>(boneCount) * 2u); // GuardedCopy
        ++counters.checks;
        LatePoseChange change;
        if (!shortcuts)
        {
            change = CompareLatePose(finalizer, work.late.data(), work.late.data() + boneCount, parents, boneCount,
                rootScale, kTranslation, kRotation, work.scratch);
        }
        else
        {
            bool equalBlocks = false;
            change = CompareLatePoseChanged(finalizer, finalizer + boneCount, work.late.data(),
                work.late.data() + boneCount, parents, boneCount, rootScale, kTranslation, kRotation, work.scratch,
                work.mask, equalBlocks);
            if (equalBlocks)
                ++counters.equal;
            if (counters.checks % 16 == 1)
            {
                const LatePoseChange full = CompareLatePose(finalizer, work.late.data(), work.late.data() + boneCount,
                    parents, boneCount, rootScale, kTranslation, kRotation, work.scratch);
                ++counters.verified;
                if (!SameChange(full, change))
                    ++counters.mismatch;
                change = full;
            }
        }
        return change;
    }

    // The in-place check, as pose_capture.cpp runs it: the in-place
    // comparison first when LateDirectCompare allows it, the copy path
    // otherwise.
    LatePoseChange CurrentLateCompare(bool shortcuts, LateCounters& counters, const BoneQts* finalizer,
        const BoneQts* engine, const std::uint16_t* parents, std::uint32_t boneCount, float rootScale, LateWork& work)
    {
        const std::size_t bytes = static_cast<std::size_t>(boneCount) * 2u * sizeof(BoneQts);
        const bool direct = LateDirectCompare(shortcuts, counters.checks) && LateBlocksEqual(engine, finalizer, bytes);
        if (!direct)
            work.late.assign(engine, engine + static_cast<std::size_t>(boneCount) * 2u);
        ++counters.checks;
        LatePoseChange change;
        if (direct)
        {
            ++counters.equal;
            ++counters.direct;
        }
        else if (!shortcuts)
        {
            change = CompareLatePose(finalizer, work.late.data(), work.late.data() + boneCount, parents, boneCount,
                rootScale, kTranslation, kRotation, work.scratch);
        }
        else
        {
            bool equalBlocks = false;
            change = CompareLatePoseChanged(finalizer, finalizer + boneCount, work.late.data(),
                work.late.data() + boneCount, parents, boneCount, rootScale, kTranslation, kRotation, work.scratch,
                work.mask, equalBlocks);
            if (equalBlocks)
                ++counters.equal;
            if (IsLateVerifyCheck(counters.checks))
            {
                const LatePoseChange full = CompareLatePose(finalizer, work.late.data(), work.late.data() + boneCount,
                    parents, boneCount, rootScale, kTranslation, kRotation, work.scratch);
                ++counters.verified;
                if (!SameChange(full, change))
                    ++counters.mismatch;
                change = full;
            }
        }
        return change;
    }

    // A finalizer output: random locals, objects composed with the table.
    std::vector<BoneQts> FinalizerOutput(const std::vector<BoneQts>& locals, const std::vector<std::uint16_t>& parents,
        float rootScale)
    {
        const std::size_t boneCount = locals.size();
        std::vector<BoneQts> output(boneCount * 2u);
        std::copy(locals.begin(), locals.end(), output.begin());
        ComposeEngineObjects(parents.data(), static_cast<std::uint32_t>(boneCount), rootScale, output.data(),
            output.data() + boneCount);
        return output;
    }

    // What the engine buffer looks like at the end of the frame, from the
    // finalizer's output: mostly untouched (every check in town), else one
    // of the writes the end-of-frame check exists for, or a byte change.
    void EndOfFrame(std::vector<BoneQts>& engine, const std::vector<BoneQts>& next, std::uint32_t boneCount,
        std::mt19937& rng, int& variant)
    {
        std::uniform_int_distribution<int> pickVariant(0, 19);
        variant = pickVariant(rng);
        if (variant > 9)
            variant = 0; // untouched, about 55% of the checks
        BoneQts* locals = engine.data();
        BoneQts* objects = engine.data() + boneCount;
        const std::uint32_t pick = 1 + static_cast<std::uint32_t>(rng() % (boneCount - 1));
        switch (variant)
        {
        case 0:
            break;
        case 1: // one byte, anywhere (the first and the last byte included)
        {
            const std::size_t bytes = engine.size() * sizeof(BoneQts);
            const std::size_t at = rng() % 3 == 0 ? (rng() % 2 ? 0 : bytes - 1) : rng() % bytes;
            reinterpret_cast<std::uint8_t*>(engine.data())[at] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
            break;
        }
        case 2: // a local rewritten after the finalizer (IK)
            locals[pick].rotation = Normalize(Multiply(locals[pick].rotation, AxisAngle(0, 0, 1, 0.05f)));
            break;
        case 3: // objects written directly (ragdoll)
            for (int k = 0; k < 3; ++k)
                objects[1 + rng() % (boneCount - 1)].translation[0] += 0.02f;
            break;
        case 4: // under the tolerances, many bones
            for (std::uint32_t bone = 0; bone < boneCount; bone += 3)
            {
                locals[bone].translation[1] += 1e-4f;
                objects[bone].translation[2] += 1e-4f;
            }
            break;
        case 5: // a NaN
            locals[pick].translation[1] = std::numeric_limits<float>::quiet_NaN();
            break;
        case 6: // the same value, other bytes: -0 for +0
            objects[pick].translation[3] = -0.0f;
            break;
        case 7: // bone 0
            locals[0].translation[0] += 0.05f;
            break;
        case 8: // the last bone's object
            objects[boneCount - 1].scale[2] += 0.5f;
            break;
        default: // a new animation frame
            engine = next;
            break;
        }
    }

    struct LateRun
    {
        std::uint64_t checks = 0;
        std::uint64_t direct = 0;
        std::uint64_t equalBytes = 0;
        std::uint64_t changed = 0;
        std::uint64_t differ = 0;
    };

    // One record's sequence of checks over finalizer outputs (each with an
    // end-of-frame buffer made from it), both paths side by side.
    void RunLateSequence(const std::vector<std::vector<BoneQts>>& outputs, const std::vector<std::uint16_t>& parents,
        float rootScale, bool shortcuts, std::mt19937& rng, LateRun& run)
    {
        if (outputs.empty())
            return;
        const std::uint32_t boneCount = static_cast<std::uint32_t>(outputs[0].size() / 2u);
        LateCounters reference, current;
        LateWork referenceWork, currentWork;
        std::vector<BoneQts> engine;
        for (std::size_t index = 0; index < outputs.size(); ++index)
        {
            const std::vector<BoneQts>& finalizer = outputs[index];
            engine = finalizer;
            int variant = 0;
            EndOfFrame(engine, outputs[(index + 1) % outputs.size()], boneCount, rng, variant);
            const LatePoseChange expected = ReferenceLateCompare(shortcuts, reference, finalizer.data(), engine.data(),
                parents.data(), boneCount, rootScale, referenceWork);
            const LatePoseChange actual = CurrentLateCompare(shortcuts, current, finalizer.data(), engine.data(),
                parents.data(), boneCount, rootScale, currentWork);
            ++run.checks;
            const bool equalBytes = std::memcmp(engine.data(), finalizer.data(), engine.size() * sizeof(BoneQts)) == 0;
            run.equalBytes += equalBytes ? 1u : 0u;
            run.changed += expected.localBones + expected.objectBones > 0 ? 1u : 0u;
            if (!SameChange(expected, actual))
            {
                if (++run.differ < 5)
                {
                    std::fprintf(stderr, "  late check %zu (%u bones, variant %d): %u/%u bones against %u/%u\n", index,
                        boneCount, variant, actual.localBones, actual.objectBones, expected.localBones,
                        expected.objectBones);
                }
            }
        }
        run.direct += current.direct;
        if (reference.checks != current.checks || reference.equal != current.equal ||
            reference.verified != current.verified || reference.mismatch != current.mismatch)
        {
            ++run.differ;
            std::fprintf(stderr, "  late counters: checks %llu/%llu, equal %llu/%llu, verified %llu/%llu\n",
                static_cast<unsigned long long>(current.checks), static_cast<unsigned long long>(reference.checks),
                static_cast<unsigned long long>(current.equal), static_cast<unsigned long long>(reference.equal),
                static_cast<unsigned long long>(current.verified), static_cast<unsigned long long>(reference.verified));
        }
        // Settled in place: exactly the non-verification checks over equal
        // bytes, with the shortcuts on.
        if (!shortcuts && current.direct != 0)
            ++run.differ;
    }

    std::vector<std::uint16_t> RandomParents(std::uint32_t boneCount, std::mt19937& rng)
    {
        std::vector<std::uint16_t> parents(boneCount, 0xFFFF);
        for (std::uint32_t bone = 1; bone < boneCount; ++bone)
        {
            // Mostly an earlier bone; a few keep their local (no parent),
            // and a few name a later bone (not composed by the finalizer).
            const std::uint32_t roll = static_cast<std::uint32_t>(rng() % 20);
            if (roll == 0)
                parents[bone] = 0xFFFF;
            else if (roll == 1)
                parents[bone] = static_cast<std::uint16_t>((std::min)(boneCount - 1, bone + 1));
            else
                parents[bone] = static_cast<std::uint16_t>(rng() % bone);
        }
        return parents;
    }

    std::vector<BoneQts> RandomLocals(std::uint32_t boneCount, std::mt19937& rng)
    {
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        std::vector<BoneQts> locals(boneCount);
        for (BoneQts& bone : locals)
        {
            bone.rotation = Normalize({unit(rng), unit(rng), unit(rng), unit(rng) + 1.5f});
            for (int axis = 0; axis < 3; ++axis)
                bone.translation[axis] = 0.3f * unit(rng);
        }
        return locals;
    }

    void TestLateDecisionRule()
    {
        // LateDirectCompare against the reference's verification rule
        // (`lateChecks % 16 == 1` after the increment).
        std::uint64_t mismatches = 0, directs = 0;
        for (std::uint64_t before = 0; before < 100000; ++before)
        {
            const bool verification = (before + 1) % 16 == 1;
            if (LateDirectCompare(true, before) != !verification || LateDirectCompare(false, before))
                ++mismatches;
            if (IsLateVerifyCheck(before + 1) != verification)
                ++mismatches;
            directs += LateDirectCompare(true, before) ? 1u : 0u;
        }
        for (const std::uint64_t before : {~0ull - 1, ~0ull - 16, (1ull << 63) - 1})
        {
            if (LateDirectCompare(true, before) != ((before + 1) % 16 != 1))
                ++mismatches;
        }
        Check(mismatches == 0, "item 6: the in-place comparison skips exactly the verification checks");
        Check(directs == 100000 - 100000 / 16, "item 6: 15 checks in 16 may be settled in place");
        // LateBlocksEqual is one memcmp over locals then objects: equal
        // exactly when both of CompareLatePoseChanged's blocks are.
        std::mt19937 rng(606);
        std::uint64_t blockMismatch = 0;
        for (int trial = 0; trial < 2000; ++trial)
        {
            const std::uint32_t boneCount = 1 + static_cast<std::uint32_t>(rng() % 64);
            std::vector<BoneQts> a = RandomLocals(boneCount * 2u, rng);
            std::vector<BoneQts> b = a;
            if (rng() % 2)
                reinterpret_cast<std::uint8_t*>(b.data())[rng() % (b.size() * sizeof(BoneQts))] ^= 0x80;
            const std::size_t half = static_cast<std::size_t>(boneCount) * sizeof(BoneQts);
            const bool blocks = std::memcmp(a.data(), b.data(), half) == 0 &&
                std::memcmp(a.data() + boneCount, b.data() + boneCount, half) == 0;
            if (LateBlocksEqual(b.data(), a.data(), half * 2u) != blocks)
                ++blockMismatch;
        }
        Check(blockMismatch == 0, "item 6: LateBlocksEqual is the two block comparisons");
        // The zero change: what CompareLatePoseChanged returns for a copy of
        // the finalizer's bytes, and what the settled check reports.
        const LatePoseChange zero{};
        Check(zero.localBones == 0 && zero.objectBones == 0 && zero.worstBone == -1 && zero.maxTranslation == 0.0f &&
                !std::signbit(zero.maxTranslation) && zero.maxRotation == 0.0f && !std::signbit(zero.maxRotation),
            "item 6: the settled change has no bones, worst bone -1 and maxima +0");
    }

    void TestLateSynthetic()
    {
        std::mt19937 rng(6006);
        LateRun runs[2];
        for (const bool shortcuts : {true, false})
        {
            LateRun& run = runs[shortcuts ? 0 : 1];
            for (const float rootScale : {1.0f, 0.919f})
            {
                for (const std::uint32_t boneCount : {2u, 94u, 671u, 1041u})
                {
                    const std::vector<std::uint16_t> parents = RandomParents(boneCount, rng);
                    std::vector<std::vector<BoneQts>> outputs;
                    for (int frame = 0; frame < 120; ++frame)
                    {
                        std::vector<BoneQts> output = FinalizerOutput(RandomLocals(boneCount, rng), parents, rootScale);
                        if (frame == 7)
                            output[boneCount / 2].translation[0] = std::numeric_limits<float>::quiet_NaN(); // NaN on both sides
                        outputs.push_back(std::move(output));
                    }
                    RunLateSequence(outputs, parents, rootScale, shortcuts, rng, run);
                }
            }
            std::printf("  late check (synthetic, shortcuts %s): %llu checks (%llu equal bytes, %llu changed), %llu "
                        "settled in place, %llu differ\n",
                shortcuts ? "on" : "off", static_cast<unsigned long long>(run.checks),
                static_cast<unsigned long long>(run.equalBytes), static_cast<unsigned long long>(run.changed),
                static_cast<unsigned long long>(run.direct), static_cast<unsigned long long>(run.differ));
        }
        Check(runs[0].differ == 0 && runs[1].differ == 0,
            "item 6: the in-place check gives the copy path's change and counters (synthetic)");
        Check(runs[0].direct > 0 && runs[1].direct == 0, "item 6: settled in place only with the shortcuts");
    }

    // Per check on an unchanged buffer: the copy and CompareLatePoseChanged
    // against the in-place comparison. Both buffers are in the cache here; in
    // the game the engine's is not (measured: the copy 10.5 us and the
    // comparison 16 us per check, rec.pose_late_copy and _compare).
    void BenchLateCheck()
    {
        std::mt19937 rng(66);
        for (const std::uint32_t boneCount : {671u, 1041u})
        {
            const std::vector<std::uint16_t> parents = RandomParents(boneCount, rng);
            const std::vector<BoneQts> finalizer = FinalizerOutput(RandomLocals(boneCount, rng), parents, 1.0f);
            const std::vector<BoneQts> engine = finalizer;
            LateWork work;
            double copyUs = 1e30, directUs = 1e30;
            std::uint64_t sink = 0;
            constexpr int kChecks = 400;
            // Through a volatile pointer: the same inputs every check, so a
            // direct call would be computed once.
            bool (*volatile blocksEqual)(const void*, const void*, std::size_t) = &LateBlocksEqual;
            for (int round = 0; round < 5; ++round)
            {
                const double t0 = NowUs();
                for (int check = 0; check < kChecks; ++check)
                {
                    work.late.assign(engine.begin(), engine.end());
                    bool equalBlocks = false;
                    const LatePoseChange change = CompareLatePoseChanged(finalizer.data(), finalizer.data() + boneCount,
                        work.late.data(), work.late.data() + boneCount, parents.data(), boneCount, 1.0f, kTranslation,
                        kRotation, work.scratch, work.mask, equalBlocks);
                    sink += change.localBones + (equalBlocks ? 1u : 0u);
                }
                const double t1 = NowUs();
                for (int check = 0; check < kChecks; ++check)
                    sink += blocksEqual(engine.data(), finalizer.data(), engine.size() * sizeof(BoneQts)) ? 1u : 0u;
                const double t2 = NowUs();
                copyUs = (std::min)(copyUs, (t1 - t0) / kChecks);
                directUs = (std::min)(directUs, (t2 - t1) / kChecks);
            }
            std::printf("  late check micro-benchmark, %u bones, unchanged buffer: copy and compare %.2f us, in place "
                        "%.2f us per check (sink %llu)\n",
                boneCount, copyUs, directUs, static_cast<unsigned long long>(sink % 7));
        }
    }

#ifdef _WIN32
    // The recorded clips: each track's first frames decoded, their locals
    // composed into a finalizer output with the layout's earlier-bone
    // parents (bytes as the game makes them), then the same sequences.
    void TestLateClips()
    {
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
            {
                std::fprintf(stderr, "  late check clip %ls: not loaded (%s)\n", entry.path().filename().c_str(),
                    error.c_str());
                continue;
            }
            std::mt19937 rng(616);
            LateRun runs[2];
            std::size_t tracks = 0;
            for (const PoseRecordExport& record : poses)
            {
                const TrackExport& track = record.track;
                if (track.chunks.empty())
                    continue;
                const TrackChunkExport& chunk = track.chunks.front();
                const SkeletonLayout& layout = track.layouts[chunk.layout];
                const std::uint32_t boneCount = layout.boneCount;
                if (boneCount < 2)
                    continue;
                std::vector<std::uint16_t> parents(boneCount, 0xFFFF);
                for (std::uint32_t bone = 1; bone < boneCount; ++bone)
                {
                    const std::int32_t parent = layout.parent[bone];
                    if (parent >= 0 && static_cast<std::uint32_t>(parent) < bone)
                        parents[bone] = static_cast<std::uint16_t>(parent);
                }
                PoseDecoder decoder;
                decoder.Begin(layout, track.config);
                std::size_t consumed = 0;
                bool decoded = true;
                if (chunk.anchor >= 0)
                {
                    const std::vector<std::uint8_t>& anchor = track.anchors[static_cast<std::size_t>(chunk.anchor)];
                    decoded = decoder.Apply(anchor.data(), anchor.size(), consumed);
                }
                std::vector<std::vector<BoneQts>> outputs;
                for (std::size_t frame = 0; decoded && frame < chunk.frameOffsets.size() && outputs.size() < 48; ++frame)
                {
                    const std::size_t offset = chunk.frameOffsets[frame];
                    if (!decoder.Apply(chunk.bytes.data() + offset, chunk.bytes.size() - offset, consumed))
                        break;
                    const std::vector<BoneQts>& channels = decoder.Channels();
                    const std::vector<BoneQts> locals(channels.begin(), channels.begin() + boneCount);
                    outputs.push_back(FinalizerOutput(locals, parents, 1.0f));
                }
                if (outputs.size() < 2)
                    continue;
                ++tracks;
                RunLateSequence(outputs, parents, 1.0f, true, rng, runs[0]);
                RunLateSequence(outputs, parents, 1.0f, false, rng, runs[1]);
            }
            std::printf("  late check clip %ls: %zu tracks, shortcuts on %llu checks (%llu settled in place), off %llu "
                        "checks, %llu differ\n",
                entry.path().filename().c_str(), tracks, static_cast<unsigned long long>(runs[0].checks),
                static_cast<unsigned long long>(runs[0].direct), static_cast<unsigned long long>(runs[1].checks),
                static_cast<unsigned long long>(runs[0].differ + runs[1].differ));
            Check(runs[0].differ == 0 && runs[1].differ == 0,
                "item 6: the in-place check gives the copy path's change and counters (recorded clip)");
        }
    }
#endif

    // ---------------------------------------------------------------------
    // Stall polls.
    void TestStallPollDue()
    {
        constexpr std::uint64_t kN = 1000; // ticks
        struct Case
        {
            std::uint64_t calls, last, now;
            bool due;
        };
        const Case cases[] = {
            {0, 0, 5, true},           // never visited
            {0, 4, 5, true},           // no call counted yet: as never visited
            {3, 0, 5, true},           // a count without a time yet
            {3, 100, 1099, false},     // 999 ticks old
            {3, 100, 1100, true},      // exactly N
            {3, 100, 5000, true},      // long ago (a corpse)
            {3, 5000, 4990, false},    // a visit just after our clock read
            {3, 1, ~0ull, true},       // no overflow
            {3, ~0ull - 5, ~0ull, false},
        };
        int failures = 0;
        for (const Case& c : cases)
            failures += StallPollDue(c.calls, c.last, kN, c.now) != c.due ? 1 : 0;
        Check(failures == 0, "D1: StallPollDue's table");
        static_assert(StallPollDue(0, 0, 10, 0), "never visited is due at once");
    }

    // One record's script-thread polls (pose_capture.cpp PollIfStalledArmed)
    // over a simulated recording: finalizer calls at `hz` with jitter that
    // stop at `stopMs` (a ragdoll, a corpse), script ticks at ~52 fps.
    struct PollSim
    {
        std::uint64_t ticks = 0;
        std::uint64_t oldRule = 0;    // ticks the plain per-tick rule polls
        std::uint64_t polls = 0;      // polls the current code takes
        std::uint64_t stallPolls = 0;
        std::uint64_t skipped = 0;
        std::uint64_t tooRecent = 0;  // stall polls with a call younger than N (must stay 0)
        std::uint64_t corpseDelayMs = 0; // the first poll after the last call, ms after it
    };

    PollSim SimulatePolls(double hz, double stopMs, std::uint64_t stallMs, std::uint32_t seed)
    {
        constexpr double kTicksPerMs = 10000.0; // QPC at 10 MHz
        const std::uint64_t stallTicks = static_cast<std::uint64_t>(static_cast<double>(stallMs) * kTicksPerMs);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> jitter(-0.3, 0.3);
        std::uniform_real_distribution<double> tickMs(17.0, 21.5);
        PollSim sim;
        std::uint64_t calls = 0, lastCall = 0, seen = ~0ull;
        std::vector<std::uint64_t> pollTimes;
        double nextCallMs = 3.0;
        for (double nowMs = 0.0; nowMs < 20000.0; nowMs += tickMs(rng))
        {
            while (nextCallMs <= nowMs && nextCallMs < stopMs)
            {
                ++calls;
                lastCall = static_cast<std::uint64_t>(nextCallMs * kTicksPerMs);
                nextCallMs += 1000.0 / hz * (1.0 + jitter(rng));
            }
            const std::uint64_t now = static_cast<std::uint64_t>(nowMs * kTicksPerMs);
            ++sim.ticks;
            sim.oldRule += calls == seen ? 1u : 0u;
            // PollIfStalledArmed's branches.
            if (calls == seen)
            {
                if (stallTicks == 0)
                {
                    ++sim.polls;
                    pollTimes.push_back(now);
                }
                else if (StallPollDue(calls, lastCall, stallTicks, now))
                {
                    ++sim.polls;
                    ++sim.stallPolls;
                    pollTimes.push_back(now);
                    if (calls != 0 && now - lastCall < stallTicks)
                        ++sim.tooRecent;
                }
                else
                {
                    ++sim.skipped;
                }
            }
            seen = calls;
        }
        // The corpse: lastCall is the last call of all (none after stopMs).
        for (const std::uint64_t time : pollTimes)
        {
            if (time > lastCall)
            {
                sim.corpseDelayMs = static_cast<std::uint64_t>(static_cast<double>(time - lastCall) / kTicksPerMs);
                break;
            }
        }
        return sim;
    }

    void TestStallPolls()
    {
        int failures = 0;
        std::uint64_t seed = 1;
        for (const double hz : {5.0, 12.0, 29.0, 53.0, 60.0})
        {
            PollSim baseline;
            for (const std::uint64_t stallMs : {0ull, 50ull, 100ull, 250ull})
            {
                const PollSim sim = SimulatePolls(hz, 12000.0, stallMs, static_cast<std::uint32_t>(seed));
                if (stallMs == 0)
                {
                    baseline = sim;
                    // The key at 0: the plain rule on every tick (the
                    // corpse polled from the second tick after its last call).
                    if (sim.polls != sim.oldRule || sim.skipped != 0 || sim.stallPolls != 0 || sim.corpseDelayMs > 45)
                        ++failures;
                    continue;
                }
                // N: every old-rule tick is a stall poll or a skipped one,
                // no stall poll sees a call younger than N, and the corpse is
                // polled from the first tick N after its last call.
                if (sim.stallPolls + sim.skipped != sim.oldRule || sim.tooRecent != 0 || sim.oldRule != baseline.oldRule)
                    ++failures;
                if (sim.corpseDelayMs < stallMs || sim.corpseDelayMs > stallMs + 22)
                    ++failures;
                if (hz == 12.0 && stallMs == 100)
                {
                    std::printf("  stall polls (12 calls/s for 12 s, then none, ticks at ~52 fps): key 0 %llu polls, "
                                "the corpse polled %llu ms after its last call; 100 ms %llu polls, %llu skipped, the "
                                "corpse polled %llu ms after\n",
                        static_cast<unsigned long long>(baseline.polls),
                        static_cast<unsigned long long>(baseline.corpseDelayMs),
                        static_cast<unsigned long long>(sim.stallPolls), static_cast<unsigned long long>(sim.skipped),
                        static_cast<unsigned long long>(sim.corpseDelayMs));
                }
            }
            ++seed;
        }
        Check(failures == 0, "D1: the key at 0 polls on the old rule's ticks; N polls only stalled records");
    }

    // The key's default (kDefaultStallPollMs, 250 ms): an
    // NPC off the game camera, which the finalizer visits 25 to 47 times a second
    // while the game runs faster, is never copied by the script (every tick the
    // plain rule would have copied, with the previous pose in the buffer, is
    // skipped); a skeleton the finalizer never visits (a posed object, a vehicle
    // it does not finalize) is copied on every tick from the second one, as the
    // plain rule does; a corpse from 250 ms after its last call.
    void TestStallPollDefault()
    {
        static_assert(kDefaultStallPollMs == 250, "PoseStallPollMs defaults to 250 ms");
        int failures = 0;
        std::uint32_t seed = 100;
        for (const double hz : {25.0, 35.0, 47.0})
        {
            const PollSim sim = SimulatePolls(hz, 20000.0, kDefaultStallPollMs, seed++);
            if (sim.oldRule == 0 || sim.stallPolls != 0 || sim.skipped != sim.oldRule)
                ++failures;
        }
        const PollSim never = SimulatePolls(30.0, 0.0, kDefaultStallPollMs, seed++);
        if (never.ticks < 2 || never.stallPolls != never.ticks - 1 || never.skipped != 0 ||
            never.polls != never.oldRule)
            ++failures;
        const PollSim corpse = SimulatePolls(35.0, 12000.0, kDefaultStallPollMs, seed++);
        if (corpse.corpseDelayMs < kDefaultStallPollMs || corpse.corpseDelayMs > kDefaultStallPollMs + 22 ||
            corpse.tooRecent != 0)
            ++failures;
        std::printf("  stall polls at the default (%u ms): off camera NPC at 35 calls/s, 0 script copies; never "
                    "visited, %llu of %llu ticks copied; the corpse copied %llu ms after its last call\n",
            static_cast<unsigned>(kDefaultStallPollMs), static_cast<unsigned long long>(never.stallPolls),
            static_cast<unsigned long long>(never.ticks), static_cast<unsigned long long>(corpse.corpseDelayMs));
        Check(failures == 0, "D1: the default 250 ms leaves the off camera NPCs to the detour and polls the "
                             "never visited skeletons and the corpses");
    }

    // ---------------------------------------------------------------------
    // Late backoff.
    void TestLateBackoff()
    {
        int failures = 0;
        // Without the backoff: the fixed wait, the clean wait untouched.
        for (const std::uint64_t start : {0ull, 250ull, 1000ull, 4000ull, 8000ull})
        {
            for (int combo = 0; combo < 4; ++combo)
            {
                const bool changed = (combo & 1) != 0;
                const bool lateCapture = (combo & 2) != 0;
                std::uint64_t clean = start;
                const std::uint64_t wait = NextLateCheckMs(false, changed, lateCapture, clean);
                const bool fast = changed || lateCapture;
                if (wait != (fast ? 250u : 1000u) || clean != start)
                    ++failures;
            }
        }
        // With it: clean checks 1, 2, 4, 8, 8 s.
        std::uint64_t clean = kLateCheckMs;
        const std::uint64_t cleanWaits[] = {1000, 2000, 4000, 8000, 8000, 8000};
        for (const std::uint64_t expected : cleanWaits)
            failures += NextLateCheckMs(true, false, false, clean) != expected ? 1 : 0;
        // A change: 250 ms, and the clean waits start again at 1 s.
        failures += NextLateCheckMs(true, true, false, clean) != 250 ? 1 : 0;
        failures += NextLateCheckMs(true, false, false, clean) != 1000 ? 1 : 0;
        failures += NextLateCheckMs(true, false, false, clean) != 2000 ? 1 : 0;
        // A change that puts the record in end-of-frame capture: 250 ms while
        // it stays (eight clean checks), then 1 s again once it leaves.
        failures += NextLateCheckMs(true, true, true, clean) != 250 ? 1 : 0;
        for (int check = 0; check < 7; ++check)
            failures += NextLateCheckMs(true, false, true, clean) != 250 ? 1 : 0;
        failures += NextLateCheckMs(true, false, false, clean) != 1000 ? 1 : 0; // the eighth: it left
        failures += NextLateCheckMs(true, false, false, clean) != 2000 ? 1 : 0;
        // A capped change (no end-of-frame capture): 250 ms, then 1 s.
        failures += NextLateCheckMs(true, true, false, clean) != 250 ? 1 : 0;
        failures += NextLateCheckMs(true, false, false, clean) != 1000 ? 1 : 0;
        // Random outcomes: without the backoff always the old wait; with it
        // never more than 8 s, never less than 250 ms.
        std::mt19937 rng(222);
        std::uint64_t cleanA = kLateCheckMs, cleanB = kLateCheckMs, maxWait = 0;
        for (int check = 0; check < 100000; ++check)
        {
            const bool changed = rng() % 9 == 0;
            const bool lateCapture = rng() % 5 == 0;
            const std::uint64_t old = NextLateCheckMs(false, changed, lateCapture, cleanA);
            if (old != ((changed || lateCapture) ? kLateCheckChangedMs : kLateCheckMs))
                ++failures;
            const std::uint64_t wait = NextLateCheckMs(true, changed, lateCapture, cleanB);
            if (wait < kLateCheckChangedMs || wait > kLateBackoffMaxMs)
                ++failures;
            maxWait = (std::max)(maxWait, wait);
        }
        failures += cleanA != kLateCheckMs ? 1 : 0;
        Check(failures == 0, "D2: NextLateCheckMs keeps the old waits without the backoff and backs off with it");
        // Checks of a record that stays clean for a minute.
        std::uint64_t elapsed = 0, checks = 0;
        clean = kLateCheckMs;
        while (elapsed < 60000)
        {
            ++checks;
            elapsed += NextLateCheckMs(true, false, false, clean);
        }
        std::printf("  late check backoff: a clean record checks 60 -> %llu times a minute, longest wait %llu ms\n",
            static_cast<unsigned long long>(checks), static_cast<unsigned long long>(maxWait));
    }

    void TestFirstCheckStagger()
    {
        // The records of one scan: ids as the recorder packs them (entity
        // slot << 16 | instance) and pose segment ids (bit 31 and a count).
        std::mt19937 rng(77);
        std::vector<std::uint32_t> ids;
        for (int index = 0; index < 50; ++index)
            ids.push_back((static_cast<std::uint32_t>(rng() % 1500) << 16) | (1 + rng() % 400));
        for (std::uint32_t index = 0; index < 10; ++index)
            ids.push_back(0x80000000u | (index + 1));
        int failures = 0;
        int bucket[60] = {}; // ticks of 1000/60 ms
        for (const std::uint32_t id : ids)
        {
            const std::uint32_t delay = LateFirstCheckDelayMs(id);
            failures += delay >= 1000 || delay != LateFirstCheckDelayMs(id) ? 1 : 0;
            ++bucket[(std::min)(59u, delay * 60u / 1000u)];
        }
        const int most = *std::max_element(std::begin(bucket), std::end(bucket));
        const int used = static_cast<int>(std::count_if(std::begin(bucket), std::end(bucket), [](int n) { return n > 0; }));
        // Consecutive instances of one slot (the same entity's segments).
        int bucketRun[60] = {};
        for (std::uint32_t instance = 1; instance <= 60; ++instance)
            ++bucketRun[(std::min)(59u, LateFirstCheckDelayMs((7u << 16) | instance) * 60u / 1000u)];
        const int mostRun = *std::max_element(std::begin(bucketRun), std::end(bucketRun));
        std::printf("  first check stagger: 60 records of one scan in %d of 60 ticks, at most %d in one (was 60 in "
                    "one); 60 consecutive ids at most %d in one tick\n",
            used, most, mostRun);
        Check(failures == 0 && most <= 6 && used >= 25 && mostRun <= 6,
            "D2: the first check's stagger is in [0, 1000) ms and spreads a scan's records");
    }
}

// The held frames.
namespace
{
    // pose_capture.cpp's worker reduced to what touches the held frames:
    // ProcessFrame, AppendFrame's contradiction and re-parent bookkeeping,
    // AppendHeldFrames, DiscoveryMain's discoveries and re-parent jobs (pop,
    // then apply), DrainRings' visits, ReleaseRetiredRing's clearing, the
    // safety valve, and the flushes. A frame is a number (record and
    // time order), a layout a hash of how it was made (a job computed from
    // other inputs gives another layout), and the validation a fixed function
    // of (frame, layout) that contradicts a few. The reference rules are
    // written out below (`v176` false); the current ones go through
    // pose_capture_rules.h as pose_capture.cpp does.
    namespace held
    {
        using namespace replay::pose::capture;

        constexpr std::size_t kSmallCap = 48;       // kMaxPendingFrames (300), scaled down with the frames
        constexpr std::size_t kNoCap = 1u << 20;    // a cap no schedule reaches
        constexpr std::size_t kFramesKept = 4;      // kReparentFramesKept
        constexpr std::size_t kDiscoveryFrames = 3; // frames a discovery is queued with

        std::uint64_t Mix(std::uint64_t a, std::uint64_t b)
        {
            std::uint64_t x = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull + (a << 6) + (a >> 2));
            x ^= x >> 31;
            x *= 0xBF58476D1CE4E5B9ull;
            x ^= x >> 29;
            x *= 0x94D049BB133111EBull;
            x ^= x >> 32;
            return x;
        }

        std::uint64_t Hash(const std::vector<std::uint32_t>& values)
        {
            std::uint64_t hash = 0x51ED27;
            for (const std::uint32_t value : values)
                hash = Mix(hash, value);
            return Mix(hash, values.size());
        }

        struct Entry
        {
            std::uint32_t frame = 0;
            std::uint64_t layout = 0;
            bool operator==(const Entry& other) const { return frame == other.frame && layout == other.layout; }
        };

        struct Rec
        {
            std::uint32_t index = 0;
            std::uint32_t frames = 0; // what its ring gives before it is retired
            std::uint32_t next = 0;
            // Frames the detour put in the ring so far
            // (the other tests: all of them at once).
            std::uint32_t ready = 0xFFFFFFFFu;
            bool layoutReady = false;
            bool discoveryQueued = false;
            std::uint64_t layout = 0;
            std::vector<std::uint32_t> pending;
            std::vector<std::uint32_t> discoverySamples;
            std::vector<std::uint32_t> reparentFrames;
            std::vector<std::uint32_t> reparentBones;
            bool reparentQueued = false;
            bool reparentRunning = false;
            bool released = false;
            bool discoveryAfterRelease = false; // its discovery ran with the samples the release cleared
            std::uint64_t dropped = 0;
            std::vector<Entry> track;
        };

        struct Model
        {
            bool v176 = false;
            std::uint32_t contradictPct = 0;
            std::size_t holdLimit = 0; // kReparentHoldLimit, the safety valve; 0: off
            std::size_t pendingCap = kSmallCap;
            std::vector<Rec> records;
            std::deque<Rec*> discoveryQueue;
            std::deque<Rec*> reparentQueue;
            Rec* running = nullptr; // a popped re-parent job, not applied yet
            std::uint64_t runningLayout = 0;
            std::uint64_t runningResult = 0;
            std::uint64_t jobs = 0;
            std::uint64_t heldVisits = 0; // visits that appended held frames
        };

        Model MakeModel(bool v176, std::uint32_t contradictPct, std::size_t holdLimit, std::size_t pendingCap,
            const std::vector<std::uint32_t>& frames)
        {
            Model model;
            model.v176 = v176;
            model.contradictPct = contradictPct;
            model.holdLimit = holdLimit;
            model.pendingCap = pendingCap;
            model.records.resize(frames.size()); // never resized again: the queues point into it
            for (std::size_t index = 0; index < frames.size(); ++index)
            {
                model.records[index].index = static_cast<std::uint32_t>(index);
                model.records[index].frames = frames[index];
            }
            return model;
        }

        // AppendFrame: on a contradiction the frame's failing bones and the
        // frame are kept for the job, which is queued once.
        bool Append(Model& model, Rec& record, std::uint32_t frame)
        {
            const std::uint64_t hash = Mix(frame, record.layout);
            if (hash % 100u < model.contradictPct)
            {
                for (const std::uint32_t bone : {static_cast<std::uint32_t>((hash >> 8) % 16u),
                         static_cast<std::uint32_t>((hash >> 16) % 16u)})
                {
                    if (std::find(record.reparentBones.begin(), record.reparentBones.end(), bone) ==
                        record.reparentBones.end())
                    {
                        record.reparentBones.push_back(bone);
                    }
                }
                if (record.reparentFrames.size() >= kFramesKept)
                    record.reparentFrames.erase(record.reparentFrames.begin());
                record.reparentFrames.push_back(frame);
                if (!record.reparentQueued)
                {
                    record.reparentQueued = true;
                    model.reparentQueue.push_back(&record);
                }
                return false;
            }
            record.track.push_back({frame, record.layout});
            return true;
        }

        // The reference AppendHeldFrames: all of them, up to a contradiction.
        void AppendHeld175(Model& model, Rec& record)
        {
            std::size_t appended = 0;
            for (; appended < record.pending.size(); ++appended)
            {
                if (!Append(model, record, record.pending[appended]))
                    break;
            }
            record.pending.erase(record.pending.begin(), record.pending.begin() + static_cast<std::ptrdiff_t>(appended));
        }

        std::size_t AppendHeld176(Model& model, Rec& record, std::size_t most)
        {
            return AppendHeldPrefix(record.pending, most,
                [&model, &record](std::uint32_t frame) { return Append(model, record, frame); });
        }

        // The appends of a discovery, a job and the safety valve.
        void AppendHeld(Model& model, Rec& record)
        {
            if (model.v176)
                AppendHeld176(model, record, kHeldAppendPerHold);
            else
                AppendHeld175(model, record);
        }

        bool Appendable(const Rec& record)
        {
            return HeldFramesAppendable(record.layoutReady, record.reparentQueued, record.reparentRunning,
                !record.pending.empty());
        }

        void Hold(const Model& model, Rec& record, std::uint32_t frame)
        {
            if (record.pending.size() < model.pendingCap)
                record.pending.push_back(frame);
            else
                ++record.dropped;
        }

        // ProcessFrame.
        void Process(Model& model, Rec& record, std::uint32_t frame)
        {
            if (record.layoutReady)
            {
                const bool direct = model.v176 ? DrainedFrameDirect(record.reparentQueued, !record.pending.empty())
                                               : !record.reparentQueued;
                if (direct && Append(model, record, frame))
                    return;
                Hold(model, record, frame);
                if (model.holdLimit != 0 && record.reparentQueued && record.pending.size() >= model.holdLimit &&
                    !record.reparentBones.empty())
                {
                    record.layout = Mix(record.layout, Mix(0xDE307Eu, Hash(record.reparentBones))); // DemoteBones
                    record.reparentBones.clear();
                    record.reparentQueued = false;
                    AppendHeld(model, record);
                }
                return;
            }
            Hold(model, record, frame);
            if (!record.discoveryQueued && record.pending.size() >= kDiscoveryFrames)
            {
                record.discoveryQueued = true;
                record.discoverySamples.assign(record.pending.begin(), record.pending.begin() + kDiscoveryFrames);
                model.discoveryQueue.push_back(&record);
            }
        }

        // ReleaseRetiredRing, once the record's ring is drained.
        void Release(Model& model, Rec& record)
        {
            if (record.released)
                return;
            if (model.v176 && Appendable(record))
                return; // current rules: after the held frames
            record.released = true;
            if (!record.reparentQueued)
            {
                record.discoverySamples.clear();
                record.reparentFrames.clear();
            }
        }

        // DrainRings' visit to one record: (current rules) held frames first, then
        // one ring frame, or the ring's release once the ring is done.
        bool Visit(Model& model, Rec& record)
        {
            bool progressed = false;
            if (model.v176 && Appendable(record) && AppendHeld176(model, record, kHeldAppendPerHold) != 0)
            {
                progressed = true;
                ++model.heldVisits;
            }
            if (record.next < record.frames && record.next < record.ready)
            {
                Process(model, record, (record.index << 20) | record.next++);
                progressed = true;
            }
            else if (record.next >= record.frames)
            {
                Release(model, record);
            }
            return progressed;
        }

        // DiscoveryMain's pop of a re-parent job; false for the leftover entry
        // of a safety valve demotion.
        bool PopJob(Model& model)
        {
            Rec& record = *model.reparentQueue.front();
            model.reparentQueue.pop_front();
            record.reparentQueued = false;
            std::vector<std::uint32_t> bones;
            bones.swap(record.reparentBones);
            if (bones.empty())
                return false;
            std::vector<std::uint32_t> inputs = record.discoverySamples;
            inputs.insert(inputs.end(), record.reparentFrames.begin(), record.reparentFrames.end());
            model.running = &record;
            model.runningLayout = record.layout;
            model.runningResult = Mix(record.layout, Mix(Hash(bones), Hash(inputs)));
            if (model.v176)
                record.reparentRunning = true;
            return true;
        }

        // DiscoveryMain's apply of the popped job (its layout unless the
        // layout changed meanwhile: the layoutSerial guard), then the appends.
        void ApplyJob(Model& model)
        {
            Rec& record = *model.running;
            model.running = nullptr;
            record.reparentRunning = false;
            if (record.layout == model.runningLayout)
                record.layout = model.runningResult;
            ++model.jobs;
            AppendHeld(model, record);
        }

        void RunDiscovery(Model& model)
        {
            Rec& record = *model.discoveryQueue.front();
            model.discoveryQueue.pop_front();
            record.discoveryQueued = false;
            record.discoveryAfterRelease = record.released;
            record.layout = Mix(0xD15C0u, Hash(record.discoverySamples));
            record.layoutReady = true;
            AppendHeld(model, record);
        }

        // One step of the discovery thread (re-parent jobs first): apply the
        // popped job, or pop the next one (applied at once unless `split`),
        // or run the next discovery. False when it had nothing to do.
        bool ThreadStep(Model& model, bool split)
        {
            if (model.running)
            {
                ApplyJob(model);
                return true;
            }
            if (!model.reparentQueue.empty())
            {
                if (PopJob(model) && !split)
                    ApplyJob(model);
                return true;
            }
            if (!model.discoveryQueue.empty())
            {
                RunDiscovery(model);
                return true;
            }
            return false;
        }

        // The discovery thread's work for one record only (its queued job,
        // applied at once, else its queued discovery), so each record's job
        // and discovery come at the same point of that record's own events in
        // both rule sets, whatever the other records queued.
        bool ThreadStepFor(Model& model, Rec& record)
        {
            const auto front = [&record](std::deque<Rec*>& queue) {
                const auto found = std::find(queue.begin(), queue.end(), &record);
                if (found == queue.end())
                    return false;
                queue.erase(found);
                queue.push_front(&record);
                return true;
            };
            if (front(model.reparentQueue))
            {
                if (PopJob(model))
                    ApplyJob(model);
                return true;
            }
            if (front(model.discoveryQueue))
            {
                RunDiscovery(model);
                return true;
            }
            return false;
        }

        // FlushHeldFrames (current rules).
        bool Flush(Model& model)
        {
            for (Rec& record : model.records)
            {
                if (Appendable(record))
                    AppendHeld176(model, record, 0);
            }
            return !model.reparentQueue.empty();
        }

        // ExportRecords (current rules: after its flush).
        std::vector<std::vector<Entry>> Export(Model& model)
        {
            if (model.v176)
                Flush(model);
            std::vector<std::vector<Entry>> tracks;
            for (const Rec& record : model.records)
                tracks.push_back(record.track);
            return tracks;
        }

        // StopAll: the worker's last drain (DrainRings goes on while a pass
        // does anything), then the discovery thread until its queues are
        // empty (current rules: then its exit flush, whose contradictions queue
        // jobs it runs), then StopAll's own flush.
        void Stop(Model& model)
        {
            for (bool progressed = true; progressed;)
            {
                progressed = false;
                for (Rec& record : model.records)
                    progressed = Visit(model, record) || progressed;
            }
            for (;;)
            {
                if (ThreadStep(model, false))
                    continue;
                if (model.v176 && Flush(model))
                    continue;
                break;
            }
            if (model.v176)
                Flush(model);
        }

        enum class Event : std::uint8_t
        {
            Visit,
            Thread,
            Export,
        };

        struct Step
        {
            Event event = Event::Visit;
            std::uint8_t record = 0;
        };

        std::vector<Step> MakeSteps(std::mt19937& rng, std::size_t records)
        {
            std::vector<Step> steps(200 + rng() % 700);
            for (Step& step : steps)
            {
                const std::uint32_t roll = rng() % 100u;
                step.event = roll < 75 ? Event::Visit : roll < 93 ? Event::Thread : Event::Export;
                step.record = static_cast<std::uint8_t>(rng() % records);
            }
            return steps;
        }

        bool Caught(const Model& model)
        {
            return model.reparentQueue.empty() && model.discoveryQueue.empty() && !model.running;
        }

        bool InTimeOrder(const Rec& record)
        {
            for (std::size_t index = 1; index < record.track.size(); ++index)
            {
                if (record.track[index].frame <= record.track[index - 1].frame)
                    return false;
            }
            return true;
        }
    }

    void TestHeldFrames()
    {
        using namespace held;
        {
            std::vector<int> values = {1, 2, 3, 4, 5, 6, 7};
            int calls = 0;
            const std::size_t four = AppendHeldPrefix(values, kHeldAppendPerHold, [&calls](int) {
                ++calls;
                return true;
            });
            Check(four == 4 && calls == 4 && values.size() == 3 && values.front() == 5,
                "4a: AppendHeldPrefix appends at most `most` frames, in order");
            const std::size_t stopped = AppendHeldPrefix(values, 0, [](int value) { return value != 6; });
            Check(stopped == 1 && values.size() == 2 && values.front() == 6,
                "4a: AppendHeldPrefix stops at a contradiction, which stays held with the rest");
            const std::size_t all = AppendHeldPrefix(values, 0, [](int) { return true; });
            Check(all == 2 && values.empty(), "4a: AppendHeldPrefix with 0 appends them all");
            bool table = true;
            for (int bits = 0; bits < 16; ++bits)
            {
                const bool ready = (bits & 1) != 0;
                const bool queued = (bits & 2) != 0;
                const bool running = (bits & 4) != 0;
                const bool any = (bits & 8) != 0;
                table = table && HeldFramesAppendable(ready, queued, running, any) == (ready && any && !queued && !running);
                // Nothing held: a drained frame goes straight in exactly when
                // it did (no job queued); with frames held, never.
                table = table && DrainedFrameDirect(queued, false) == !queued && !DrainedFrameDirect(queued, true);
            }
            Check(table, "4a: HeldFramesAppendable and DrainedFrameDirect tables");
        }

        std::mt19937 rng(4004);
        constexpr std::size_t kRecords = 3;
        // A: each record's discovery-thread work at the same point of its own
        // events in both rule sets (ThreadStepFor; a re-parent job applied in
        // one step, so no frame is drained while one runs), no cap pressure
        // and no safety valve: the current tracks must be the reference ones,
        // frame for frame with the same layouts, and every export taken when
        // both are caught up must be equal.
        std::uint64_t schedulesA = 0;
        std::uint64_t framesA = 0;
        std::uint64_t jobs175 = 0;
        std::uint64_t jobs176 = 0;
        std::uint64_t heldVisits = 0;
        std::uint64_t exportsCompared = 0;
        std::uint64_t exportsDiffer = 0;
        std::uint64_t tracksDiffer = 0;
        for (int round = 0; round < 3000; ++round)
        {
            const std::uint32_t pct = 1 + rng() % 12u;
            std::vector<std::uint32_t> frames;
            for (std::size_t record = 0; record < kRecords; ++record)
                frames.push_back(10 + rng() % 140u);
            const std::vector<Step> steps = MakeSteps(rng, kRecords);
            Model v175 = MakeModel(false, pct, 0, kNoCap, frames);
            Model v176 = MakeModel(true, pct, 0, kNoCap, frames);
            for (const Step& step : steps)
            {
                if (step.event == Event::Visit)
                {
                    Visit(v175, v175.records[step.record]);
                    Visit(v176, v176.records[step.record]);
                }
                else if (step.event == Event::Thread)
                {
                    ThreadStepFor(v175, v175.records[step.record]);
                    ThreadStepFor(v176, v176.records[step.record]);
                }
                else
                {
                    const std::vector<std::vector<Entry>> a = Export(v175);
                    const std::vector<std::vector<Entry>> b = Export(v176);
                    if (Caught(v175) && Caught(v176))
                    {
                        ++exportsCompared;
                        exportsDiffer += a == b ? 0u : 1u;
                    }
                }
            }
            Stop(v175);
            Stop(v176);
            for (std::size_t record = 0; record < kRecords; ++record)
            {
                const Rec& a = v175.records[record];
                const Rec& b = v176.records[record];
                tracksDiffer += a.track == b.track && a.dropped == 0 && b.dropped == 0 && a.pending.empty() &&
                    b.pending.empty() ? 0u : 1u;
                framesA += b.frames;
            }
            jobs175 += v175.jobs;
            jobs176 += v176.jobs;
            heldVisits += v176.heldVisits;
            ++schedulesA;
        }
        std::printf("  held frames, reference rules against current (each record's jobs and discovery at the same point "
                    "of its events, applied in one step; no cap pressure, no safety valve): %llu schedules, %llu "
                    "frames, re-parent jobs %llu / %llu, %llu worker visits appended held frames; final tracks %llu "
                    "differ; exports with both caught up: %llu compared, %llu differ\n",
            static_cast<unsigned long long>(schedulesA), static_cast<unsigned long long>(framesA),
            static_cast<unsigned long long>(jobs175), static_cast<unsigned long long>(jobs176),
            static_cast<unsigned long long>(heldVisits), static_cast<unsigned long long>(tracksDiffer),
            static_cast<unsigned long long>(exportsCompared), static_cast<unsigned long long>(exportsDiffer));
        Check(tracksDiffer == 0, "4a: the incremental held appends give the reference tracks");
        Check(exportsDiffer == 0 && exportsCompared > 0, "4a: an export after the flush is the reference one");
        Check(heldVisits > 0 && jobs176 > 0 && jobs176 == jobs175, "4a: the same re-parent jobs ran");

        // A2: the same with one discovery thread for all records (first in,
        // first out, jobs before discoveries) and a 48-frame cap. A
        // contradiction among held frames is now found by the incremental
        // appends, a few worker passes after the job's hold where the reference
        // found it (milliseconds in the game, against a 300-frame cap), so
        // that record's next job is queued later and the shared thread's
        // order shifts. The tracks then differ only through the timing of
        // the thread and of the cap, as two runs of the game do: a record
        // holding its frames longer drops other frames at the cap, or a
        // record's discovery runs on the other side of its ring's release
        // (ReleaseRetiredRing clears the samples of a queued discovery).
        // Every record that differs must be one of those.
        std::uint64_t recordsA2 = 0;
        std::uint64_t differA2 = 0;
        std::uint64_t byDrops = 0;
        std::uint64_t byDiscovery = 0;
        std::uint64_t unexplained = 0;
        for (int round = 0; round < 3000; ++round)
        {
            const std::uint32_t pct = 1 + rng() % 12u;
            std::vector<std::uint32_t> frames;
            for (std::size_t record = 0; record < kRecords; ++record)
                frames.push_back(10 + rng() % 140u);
            const std::vector<Step> steps = MakeSteps(rng, kRecords);
            Model v175 = MakeModel(false, pct, 0, kSmallCap, frames);
            Model v176 = MakeModel(true, pct, 0, kSmallCap, frames);
            for (const Step& step : steps)
            {
                if (step.event == Event::Visit)
                {
                    Visit(v175, v175.records[step.record]);
                    Visit(v176, v176.records[step.record]);
                }
                else if (step.event == Event::Thread)
                {
                    ThreadStep(v175, false);
                    ThreadStep(v176, false);
                }
                else
                {
                    Export(v175);
                    Export(v176);
                }
            }
            Stop(v175);
            Stop(v176);
            for (std::size_t record = 0; record < kRecords; ++record)
            {
                const Rec& a = v175.records[record];
                const Rec& b = v176.records[record];
                ++recordsA2;
                if (a.track == b.track && a.dropped == b.dropped)
                    continue;
                ++differA2;
                if (a.dropped != b.dropped)
                    ++byDrops;
                else if (a.discoveryAfterRelease != b.discoveryAfterRelease)
                    ++byDiscovery;
                else
                    ++unexplained;
            }
        }
        std::printf("  held frames with one discovery thread for all records and a 48-frame cap: %llu records, %llu "
                    "differ (%llu dropped other frames at the cap, %llu ran their discovery on the other side of the "
                    "ring's release), %llu unexplained\n",
            static_cast<unsigned long long>(recordsA2), static_cast<unsigned long long>(differA2),
            static_cast<unsigned long long>(byDrops), static_cast<unsigned long long>(byDiscovery),
            static_cast<unsigned long long>(unexplained));
        Check(unexplained == 0, "4a: with the shared discovery thread, tracks differ only through its timing and the cap");

        // B: jobs that run while frames are drained, and the safety valve.
        // The reference appends a frame drained during a running job straight
        // to the track, ahead of the older frames the job held (reparentQueued
        // is cleared at the pop): out of time order. The current rules hold it
        // behind them: every track in time order, every frame appended or
        // dropped.
        std::uint64_t schedulesB = 0;
        std::uint64_t outOfOrder175 = 0;
        std::uint64_t outOfOrder176 = 0;
        std::uint64_t lost176 = 0;
        std::uint64_t valveJobs = 0;
        for (int round = 0; round < 3000; ++round)
        {
            const std::uint32_t pct = 1 + rng() % 12u;
            const std::size_t holdLimit = 6 + rng() % 24u;
            std::vector<std::uint32_t> frames;
            for (std::size_t record = 0; record < kRecords; ++record)
                frames.push_back(10 + rng() % 140u);
            const std::vector<Step> steps = MakeSteps(rng, kRecords);
            Model v175 = MakeModel(false, pct, holdLimit, kSmallCap, frames);
            Model v176 = MakeModel(true, pct, holdLimit, kSmallCap, frames);
            for (const Step& step : steps)
            {
                if (step.event == Event::Visit)
                {
                    Visit(v175, v175.records[step.record]);
                    Visit(v176, v176.records[step.record]);
                }
                else if (step.event == Event::Thread)
                {
                    ThreadStep(v175, true);
                    ThreadStep(v176, true);
                }
                else
                {
                    Export(v175);
                    Export(v176);
                }
            }
            Stop(v175);
            Stop(v176);
            bool ordered175 = true;
            for (std::size_t record = 0; record < kRecords; ++record)
            {
                const Rec& a = v175.records[record];
                const Rec& b = v176.records[record];
                ordered175 = ordered175 && InTimeOrder(a);
                outOfOrder176 += InTimeOrder(b) ? 0u : 1u;
                lost176 += b.pending.empty() && b.track.size() + b.dropped == b.frames ? 0u : 1u;
            }
            outOfOrder175 += ordered175 ? 0u : 1u;
            valveJobs += v176.jobs;
            ++schedulesB;
        }
        std::printf("  held frames with jobs running while frames are drained, and the safety valve: %llu schedules, "
                    "%llu re-parent jobs; current tracks out of time order %llu, frames lost %llu; reference "
                    "appended out of time order in %llu schedules\n",
            static_cast<unsigned long long>(schedulesB), static_cast<unsigned long long>(valveJobs),
            static_cast<unsigned long long>(outOfOrder176), static_cast<unsigned long long>(lost176),
            static_cast<unsigned long long>(outOfOrder175));
        Check(outOfOrder176 == 0, "4a: frames drained while held frames wait are appended after them");
        Check(lost176 == 0, "4a: every drained frame is appended or dropped by the cap, none stays held");
    }
}

// The worker's drain list, the buffer's prune of finished records, and the drain
// of every ready slot per visit.
namespace
{
    void TestDrainListRules()
    {
        bool table = true;
        for (int bits = 0; bits < 64; ++bits)
        {
            const bool b[6] = {(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0, (bits & 16) != 0,
                (bits & 32) != 0};
            table = table && RecordFinished(b[0], b[1], b[2], b[3], b[4], b[5]) ==
                (b[0] && b[1] && !b[2] && !b[3] && !b[4] && !b[5]);
        }
        Check(table, "1a: RecordFinished needs retired, ring released, nothing held, no discovery or job");
        // Held frames that can never get a
        // layout are dropped, and the record then finishes.
        bool stranded = true;
        for (int bits = 0; bits < 16; ++bits)
        {
            const bool retired = (bits & 1) != 0;
            const bool released = (bits & 2) != 0;
            const bool layoutReady = (bits & 4) != 0;
            const bool queued = (bits & 8) != 0;
            stranded = stranded && HeldFramesStranded(retired, released, layoutReady, queued) ==
                (retired && released && !layoutReady && !queued);
        }
        // A retired record whose discovery failed (or never ran) with frames
        // held: not finished before the drop, finished after it.
        stranded = stranded && !RecordFinished(true, true, true, false, false, false) &&
            HeldFramesStranded(true, true, false, false) && RecordFinished(true, true, false, false, false, false);
        // A queued discovery or a known layout keeps the held frames.
        stranded = stranded && !HeldFramesStranded(true, true, false, true) &&
            !HeldFramesStranded(true, true, true, false) && !HeldFramesStranded(true, false, false, false);
        Check(stranded, "1a: HeldFramesStranded drops held frames only when no layout can come; the record finishes");
        bool prunable = true;
        for (int bits = 0; bits < 8; ++bits)
        {
            const bool finished = (bits & 1) != 0;
            const bool queued = (bits & 2) != 0;
            const bool track = (bits & 4) != 0;
            for (const std::uint32_t last : {0u, 999u, 1000u, 1001u})
            {
                const bool expected = finished && !queued && (!track || last < 1000u);
                prunable = prunable && RecordPrunable(finished, queued, track, last, 1000u) == expected;
            }
        }
        // timeMs 0 prunes no record with a track (nothing ends before 0).
        prunable = prunable && !RecordPrunable(true, false, true, 0u, 0u);
        Check(prunable, "1c: RecordPrunable: finished, no queue entry, ended before the base (or no track)");

        std::mt19937 rng(1001);
        std::uint64_t mismatches = 0;
        for (int trial = 0; trial < 5000; ++trial)
        {
            std::vector<std::unique_ptr<int>> items;
            const std::size_t count = rng() % 40u;
            for (std::size_t index = 0; index < count; ++index)
                items.push_back(std::make_unique<int>(static_cast<int>(index)));
            const std::uint32_t pct = rng() % 101u;
            std::vector<int> expected;
            std::vector<int> removedExpected;
            std::vector<bool> drop(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                drop[index] = rng() % 100u < pct;
                (drop[index] ? removedExpected : expected).push_back(static_cast<int>(index));
            }
            std::vector<int> removed;
            const std::size_t gone = StableRemove(items,
                [&](const std::unique_ptr<int>& item) { return drop[static_cast<std::size_t>(*item)]; },
                [&](const std::unique_ptr<int>& item) { removed.push_back(*item); });
            std::vector<int> kept;
            for (const std::unique_ptr<int>& item : items)
                kept.push_back(item ? *item : -1);
            mismatches += kept == expected && removed == removedExpected && gone == removedExpected.size() ? 0u : 1u;
        }
        Check(mismatches == 0, "1c: StableRemove keeps the order of the rest and sees each removed one");
        Check(ClampWorkerPollMs(5) == 5 && ClampWorkerPollMs(0) == 1 && ClampWorkerPollMs(-3) == 1 &&
                ClampWorkerPollMs(100) == 100 && ClampWorkerPollMs(250) == 100 && kWorkerPollMsDefault == 5,
            "3d: PoseWorkerPollMs is 5 unless set, 1 to 100");
    }

    namespace drainlist
    {
        using namespace held;

        bool Finished(const Rec& record)
        {
            return RecordFinished(record.next >= record.frames, record.released, !record.pending.empty(),
                record.discoveryQueued, record.reparentQueued, record.reparentRunning);
        }

        // The reference: every record of the recording, one
        // ring frame per visit, passes while one did anything.
        std::uint64_t DrainAll(Model& model)
        {
            std::uint64_t visits = 0;
            for (bool progressed = true; progressed;)
            {
                progressed = false;
                for (Rec& record : model.records)
                {
                    ++visits;
                    progressed = Visit(model, record) || progressed;
                }
            }
            return visits;
        }

        // As pose_capture.cpp DrainRings: the drain list,
        // every ready slot of a record per visit (at most a ring's 8), the
        // release tried after a visit that drained nothing, and a finished
        // record taken off the list.
        std::uint64_t DrainList(Model& model, std::vector<Rec*>& list, std::vector<std::size_t>& sealedAt)
        {
            std::uint64_t visits = 0;
            for (bool progressed = true; progressed;)
            {
                progressed = false;
                for (std::size_t index = 0; index < list.size();)
                {
                    Rec& record = *list[index];
                    ++visits;
                    if (Appendable(record) && AppendHeld176(model, record, kHeldAppendPerHold) != 0)
                    {
                        progressed = true;
                        ++model.heldVisits;
                    }
                    std::uint32_t drained = 0;
                    while (drained < 8 && record.next < record.frames && record.next < record.ready)
                    {
                        Process(model, record, (record.index << 20) | record.next++);
                        ++drained;
                    }
                    bool finished = false;
                    if (drained != 0)
                    {
                        progressed = true;
                    }
                    else
                    {
                        if (record.next >= record.frames)
                            Release(model, record);
                        finished = Finished(record);
                    }
                    if (finished)
                    {
                        sealedAt[record.index] = record.track.size();
                        list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
                    }
                    else
                    {
                        ++index;
                    }
                }
            }
            return visits;
        }
    }

    void TestDrainListModel()
    {
        using namespace held;
        using namespace drainlist;
        std::mt19937 rng(1303);
        constexpr std::size_t kRecords = 6;
        // Both worker loops on the current held-frame rules, the
        // same frames reaching the rings at the same points, and each
        // record's discovery-thread work (ThreadStepFor) at the same point of
        // its own events, between two worker passes. No cap pressure and no
        // safety valve (both count held frames, which a batch of 8 drained
        // frames reaches in another order): the tracks must be equal frame for
        // frame with the same layouts, every export equal, and a record taken
        // off the list must never get another frame.
        std::uint64_t schedules = 0, frames = 0, tracksDiffer = 0, exportsCompared = 0, exportsDiffer = 0;
        std::uint64_t visitsAll = 0, visitsList = 0, finishedRecords = 0, changedAfterSeal = 0, jobs = 0;
        for (int round = 0; round < 3000; ++round)
        {
            const std::uint32_t pct = 1 + rng() % 12u;
            std::vector<std::uint32_t> counts;
            for (std::size_t record = 0; record < kRecords; ++record)
                counts.push_back(rng() % 4u == 0 ? rng() % 4u : 10 + rng() % 140u);
            Model all = MakeModel(true, pct, 0, kNoCap, counts);
            Model list = MakeModel(true, pct, 0, kNoCap, counts);
            for (Model* model : {&all, &list})
                for (Rec& record : model->records)
                    record.ready = 0;
            std::vector<Rec*> drain;
            for (Rec& record : list.records)
                drain.push_back(&record);
            std::vector<std::size_t> sealedAt(kRecords, ~std::size_t{0});
            const std::size_t steps = 300 + rng() % 900u;
            for (std::size_t step = 0; step < steps; ++step)
            {
                const std::uint32_t roll = rng() % 100u;
                const std::size_t record = rng() % kRecords;
                if (roll < 55)
                {
                    // The detour fills 1 to 9 slots before the next pass (a
                    // poll period of up to ~300 ms at 30 Hz).
                    const std::uint32_t more = 1 + rng() % 9u;
                    all.records[record].ready += more;
                    list.records[record].ready += more;
                }
                else if (roll < 80)
                {
                    visitsAll += DrainAll(all);
                    visitsList += DrainList(list, drain, sealedAt);
                }
                else if (roll < 95)
                {
                    ThreadStepFor(all, all.records[record]);
                    ThreadStepFor(list, list.records[record]);
                }
                else
                {
                    ++exportsCompared;
                    exportsDiffer += Export(all) == Export(list) ? 0u : 1u;
                }
            }
            for (Model* model : {&all, &list})
                for (Rec& record : model->records)
                    record.ready = record.frames;
            visitsAll += DrainAll(all);
            visitsList += DrainList(list, drain, sealedAt);
            Stop(all);
            Stop(list);
            for (std::size_t record = 0; record < kRecords; ++record)
            {
                const Rec& a = all.records[record];
                const Rec& b = list.records[record];
                tracksDiffer += a.track == b.track && a.dropped == b.dropped && a.pending == b.pending ? 0u : 1u;
                frames += b.frames;
                if (sealedAt[record] != ~std::size_t{0})
                {
                    ++finishedRecords;
                    changedAfterSeal += sealedAt[record] == b.track.size() ? 0u : 1u;
                }
            }
            jobs += list.jobs;
            ++schedules;
        }
        std::printf("  drain list and all ready slots per visit (items 1a, 3d) against one slot per visit over every "
                    "record: %llu schedules, %llu frames, %llu re-parent jobs; tracks %llu differ; exports %llu "
                    "compared, %llu differ; %llu records left the list, %llu got a frame after; visits %llu -> %llu\n",
            static_cast<unsigned long long>(schedules), static_cast<unsigned long long>(frames),
            static_cast<unsigned long long>(jobs), static_cast<unsigned long long>(tracksDiffer),
            static_cast<unsigned long long>(exportsCompared), static_cast<unsigned long long>(exportsDiffer),
            static_cast<unsigned long long>(finishedRecords), static_cast<unsigned long long>(changedAfterSeal),
            static_cast<unsigned long long>(visitsAll), static_cast<unsigned long long>(visitsList));
        Check(tracksDiffer == 0 && exportsDiffer == 0 && exportsCompared > 0,
            "1a/3d: the drain list and the batch drain give the same tracks and exports");
        Check(finishedRecords > 0 && changedAfterSeal == 0, "1a: a record taken off the drain list never changes");
        Check(visitsList < visitsAll, "1a: the drain list visits fewer records");
    }

    // The prune against the real buffer and window cut: a buffer recording of
    // entities coming and going (lifecycle, 2 s blocks, 3-bone tracks at
    // 30 Hz), the head following the window with replay::buffer::TrimBefore
    // and every track's DropBefore(ExportableBaseMs) as script.cpp does; one
    // side also prunes the finished records (entity deleted, its record
    // finished a random while later) whose last frame is before that base.
    // Every save (window::Cut from the head, or from a manual mark after it)
    // must give the same pose records, byte for byte, and the same result.
    bool SameExport(const replay::pose::PoseRecordExport& a, const replay::pose::PoseRecordExport& b)
    {
        if (a.recordId != b.recordId || a.boneCount != b.boneCount || a.track.currentLayout != b.track.currentLayout ||
            a.track.layouts.size() != b.track.layouts.size() || a.track.anchors != b.track.anchors ||
            a.track.chunks.size() != b.track.chunks.size())
            return false;
        for (std::size_t index = 0; index < a.track.layouts.size(); ++index)
            if (!SameLayout(a.track.layouts[index], b.track.layouts[index]))
                return false;
        for (std::size_t index = 0; index < a.track.chunks.size(); ++index)
        {
            const replay::pose::TrackChunkExport& x = a.track.chunks[index];
            const replay::pose::TrackChunkExport& y = b.track.chunks[index];
            if (x.layout != y.layout || x.anchor != y.anchor || x.bytes.Get() != y.bytes.Get() ||
                x.frameTimes != y.frameTimes || x.frameOffsets != y.frameOffsets)
                return false;
        }
        const replay::pose::TrackStats& s = a.track.stats;
        const replay::pose::TrackStats& t = b.track.stats;
        return s.frames == t.frames && s.keyframes == t.keyframes && s.relativeKeyframes == t.relativeKeyframes &&
            s.fullKeyframeBytes == t.fullKeyframeBytes && s.relativeKeyframeBytes == t.relativeKeyframeBytes &&
            s.bytes == t.bytes && s.droppedChunks == t.droppedChunks && s.firstTimeMs == t.firstTimeMs &&
            s.lastTimeMs == t.lastTimeMs;
    }

    void TestBufferPrune()
    {
        using namespace replay;
        pose::SkeletonLayout layout;
        layout.boneCount = 3;
        layout.parent = {pose::kRootParent, 0, 1};
        layout.ambiguous = {0, 0, 0};
        layout.highPrecision = {0, 0, 0};
        layout.evaluationOrder = {0, 1, 2};
        pose::StreamConfig config;
        config.keyframeIntervalMs = 2000;

        struct Ent
        {
            entity::ReplayId id;
            std::uint32_t createMs = 0;
            std::uint32_t deleteMs = 0;
            std::uint32_t finishMs = 0;
            bool posed = true; // its record gets a track (layout)
            bool created = false;
            bool deleted = false;
            std::unique_ptr<pose::PoseTrack> track;
        };
        std::mt19937 rng(1102);
        std::uint64_t saves = 0, savesDiffer = 0, poseRecordsSaved = 0, pruned = 0, prunedNoTrack = 0;
        std::uint64_t prunedBytes = 0, records = 0, baseBelowPrune = 0;
        for (const std::uint32_t windowMs : {20000u, 60000u})
        {
            constexpr std::uint32_t kEndMs = 240000;
            Recording clip;
            clip.lifecycle.Begin();
            std::vector<Ent> ents(160);
            for (std::size_t index = 0; index < ents.size(); ++index)
            {
                Ent& e = ents[index];
                e.id = {static_cast<std::uint16_t>(index), static_cast<std::uint16_t>(index + 1)};
                e.createMs = rng() % (kEndMs - 1000);
                const std::uint32_t life = rng() % 8u == 0 ? rng() % 100u : 500 + rng() % 70000u;
                e.deleteMs = rng() % 6u == 0 ? kEndMs + 1 : e.createMs + life; // some live to the end
                e.posed = rng() % 10u != 0;
            }
            // The records in creation order, both sides (the pruned side
            // through StableRemove, as DropBefore).
            std::vector<Ent*> full;
            std::vector<Ent*> kept;
            std::uint32_t headMs = 0;
            std::uint32_t lastBlock = 0;
            std::uint32_t prunedAtMs = 0; // the latest prune base
            std::uint32_t nextSave = 3000 + rng() % 4000u;
            for (std::uint32_t t = 0; t <= kEndMs; t += 33)
            {
                clip.lifecycle.Advance(t);
                for (Ent& e : ents)
                {
                    if (!e.created && t >= e.createMs)
                    {
                        entity::CreatePacket packet;
                        packet.id = e.id;
                        packet.timeMs = t;
                        packet.kind = entity::EntityKind::Ped;
                        packet.boneCount = 3;
                        clip.lifecycle.Create(packet);
                        clip.created.push_back(packet);
                        e.created = true;
                        if (e.posed)
                        {
                            e.track = std::make_unique<pose::PoseTrack>();
                            e.track->Begin(layout, config);
                        }
                        full.push_back(&e);
                        kept.push_back(&e);
                        ++records;
                    }
                    if (e.created && !e.deleted && t >= e.deleteMs)
                    {
                        clip.lifecycle.Delete(e.id, t);
                        e.deleted = true;
                        e.finishMs = t + rng() % 5000u; // the worker finishes it a while later
                    }
                    if (e.created && !e.deleted && e.track)
                    {
                        const float s = static_cast<float>(t) / 1000.0f + static_cast<float>(e.id.slot);
                        pose::BoneQts locals[3];
                        for (std::uint32_t bone = 0; bone < 3; ++bone)
                        {
                            const float angle = s * (1.0f + static_cast<float>(bone));
                            locals[bone].rotation = pose::Normalize({0.0f, 0.0f, std::sin(angle * 0.5f), std::cos(angle * 0.5f)});
                            locals[bone].translation[0] = 0.1f * static_cast<float>(bone) + s * 0.01f;
                        }
                        pose::BoneQts objects[3];
                        pose::ComposeObjects(layout, locals, nullptr, objects);
                        float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, s, 2.0f, 3.0f, 1};
                        pose::FrameInput input;
                        input.timeMs = t;
                        input.world = world;
                        input.locals = locals;
                        input.objects = objects;
                        e.track->Append(input);
                    }
                }
                if (t / 2000u != lastBlock)
                {
                    // UpdateRecordingLimits' block: the head follows the
                    // window (ApplyBufferHead).
                    lastBlock = t / 2000u;
                    const std::uint32_t head = t > windowMs ? t - windowMs : 0u;
                    if (head > headMs)
                    {
                        buffer::TrimBefore(clip, head);
                        const std::uint32_t base = buffer::ExportableBaseMs(clip, head);
                        for (Ent* e : full)
                            if (e->track)
                                e->track->DropBefore(base);
                        pruned += StableRemove(kept,
                            [&](const Ent* e) {
                                const bool finished = e->deleted && t >= e->finishMs;
                                return RecordPrunable(finished, false, e->track != nullptr,
                                    e->track ? e->track->Stats().lastTimeMs : 0u, base);
                            },
                            [&](const Ent* e) {
                                prunedNoTrack += e->track ? 0u : 1u;
                                prunedBytes += e->track ? e->track->Bytes() : 0u;
                            });
                        prunedAtMs = base;
                        headMs = head;
                    }
                }
                if (t >= nextSave)
                {
                    nextSave = t + 1000 + rng() % 6000u;
                    // The buffer's save (from the head), or a manual mark
                    // somewhere after it.
                    const std::uint32_t from = rng() % 2u == 0 || t <= headMs ? headMs : headMs + rng() % (t - headMs);
                    const auto exportOf = [](const std::vector<Ent*>& list) {
                        std::vector<pose::PoseRecordExport> out;
                        for (const Ent* e : list)
                        {
                            if (!e->track)
                                continue;
                            pose::PoseRecordExport exported;
                            exported.recordId = e->id.Packed();
                            exported.boneCount = 3;
                            e->track->Export(exported.track);
                            out.push_back(std::move(exported));
                        }
                        return out;
                    };
                    Recording cutA;
                    Recording cutB;
                    std::vector<pose::PoseRecordExport> posesA;
                    std::vector<pose::PoseRecordExport> posesB;
                    window::Result resultA;
                    window::Result resultB;
                    const bool okA = window::Cut(clip, exportOf(full), t, from, t, cutA, posesA, resultA);
                    const bool okB = window::Cut(clip, exportOf(kept), t, from, t, cutB, posesB, resultB);
                    bool same = okA == okB && posesA.size() == posesB.size() && resultA.baseMs == resultB.baseMs &&
                        resultA.endMs == resultB.endMs && resultA.durationMs == resultB.durationMs &&
                        resultA.poseFramesClamped == resultB.poseFramesClamped &&
                        resultA.poseFramesTruncated == resultB.poseFramesTruncated && resultA.error == resultB.error &&
                        cutA.created.size() == cutB.created.size();
                    for (std::size_t index = 0; same && index < posesA.size(); ++index)
                        same = SameExport(posesA[index], posesB[index]);
                    baseBelowPrune += okA && resultA.baseMs < prunedAtMs ? 1u : 0u;
                    savesDiffer += same ? 0u : 1u;
                    poseRecordsSaved += posesA.size();
                    ++saves;
                }
            }
        }
        std::printf("  buffer prune (item 1c) against the window cut: %llu records, %llu pruned (%llu without a "
                    "track, %.1f KB of last chunks), %llu saves with %llu pose records, %llu differ, %llu cut "
                    "below a prune base\n",
            static_cast<unsigned long long>(records), static_cast<unsigned long long>(pruned),
            static_cast<unsigned long long>(prunedNoTrack), static_cast<double>(prunedBytes) / 1024.0,
            static_cast<unsigned long long>(saves), static_cast<unsigned long long>(poseRecordsSaved),
            static_cast<unsigned long long>(savesDiffer), static_cast<unsigned long long>(baseBelowPrune));
        Check(pruned > 0 && saves > 0 && savesDiffer == 0, "1c: the pruned records are exactly ones every later cut drops");
        Check(baseBelowPrune == 0, "1c: every cut's base is at or after the last prune's base");
    }

    // ---------------------------------------------------------------------
    // The capture capacity (capture_capacity.h). The defaults are the fixed
    // constants (56 peds, 64 places, 96 weapons, 80 pool handles per scan);
    // the late mask's slots below 64 map to the bits of a one-word mask.

    // The one-word SetLateBit, the reference for slots below 64.
    std::uint64_t ReferenceLateWord(std::uint64_t mask, std::uint32_t slot, bool late)
    {
        const std::uint64_t bit = 1ull << slot;
        return late ? (mask | bit) : (mask & ~bit);
    }

    template <std::uint32_t Slots>
    std::vector<std::uint32_t> SetSlots(const replay::capacity::SlotMask<Slots>& mask)
    {
        std::uint64_t words[replay::capacity::SlotMask<Slots>::kWords];
        std::vector<std::uint32_t> slots;
        if (!mask.Load(words))
            return slots;
        for (std::uint32_t word = 0; word < replay::capacity::SlotMask<Slots>::kWords; ++word)
        {
            for (std::uint32_t bit = 0; bit < 64; ++bit)
            {
                if (words[word] & (1ull << bit))
                    slots.push_back(word * 64u + bit);
            }
        }
        return slots;
    }

    void TestCaptureCapacity()
    {
        using namespace replay::capacity;
        Check(PedCapacity() == 56 && kDefaultPedCapacity == 56, "capacity: the default ped class is 56");
        Check(LiveTargets(kDefaultPedCapacity, kLegacyVehicleCapacity) == 64,
            "capacity: 64 capture places without the GTA model, as before");
        Check(LiveTargets(kLegacyMaxPedCapacity, kLegacyVehicleCapacity) == 128,
            "capacity: 120 peds and 8 vehicles still fill the old 128");
        Check(kPedPoolSlots == 150 && kMaxPedCapacity == 150 && kVehiclePoolSlots == 80 && kMaxVehicleCapacity == 79,
            "capacity: the ped pool (150) and the vehicle pool (80, 79 for the recording)");
        Check(kLiveTargetSlots == 327 && LiveTargets(kMaxPedCapacity, kMaxVehicleCapacity) <= kLiveTargetSlots,
            "capacity: 327 slots, both whole pools fit");
        Check(kLiveTargetSlots - LiveTargets(kGtaDefaultPedCapacity, kMaxVehicleCapacity) ==
                256 - LiveTargets(kGtaDefaultPedCapacity, kLegacyVehicleCapacity),
            "capacity: with the GTA model the slots left (weapon places) are the 100 of the 8-vehicle class");
        Check(VehicleCapacityFor(false) == 8 && VehicleCapacityFor(true) == 79,
            "capacity: 8 vehicles off, the vehicle pool less 1 (the hidden player's vehicle) with the GTA model");
        Check(PoolVehicleBudget(8) == 16 && PoolVehicleBudget(79) == 87,
            "capacity: 16 new vehicle handles per scan at 8, 87 (more than the pool holds) at 79");
        Check(ClampPedCapacity(-5) == 8 && ClampPedCapacity(0) == 8 && ClampPedCapacity(56) == 56 &&
                ClampPedCapacity(120) == 120 && ClampPedCapacity(500) == 120 && ClampPedCapacity(150) == 120,
            "capacity: without the GTA model PedCapacity stays 8..120");
        Check(ClampPedCapacity(-5, true) == 8 && ClampPedCapacity(56, true) == 56 && ClampPedCapacity(120, true) == 120 &&
                ClampPedCapacity(148, true) == 148 && ClampPedCapacity(150, true) == 150 && ClampPedCapacity(151, true) == 150 &&
                ClampPedCapacity(500, true) == 150,
            "capacity: with WorldModel=gta PedCapacity accepts up to the pool (150)");
        Check(DefaultPedCapacity(false) == 56 && DefaultPedCapacity(true) == 148 && PedCapacityLimit(false) == 120 &&
                PedCapacityLimit(true) == 150,
            "capacity: default 56 off, the pool less 2 (148) with the GTA model");
        Check(WeaponCapacity(56) == 96 && WeaponCapacity(8) == 96 && WeaponCapacity(120) == 206 && WeaponCapacity(150) == 258,
            "capacity: 96 weapons at 56 peds or fewer, in proportion above");
        bool monotonic = true;
        for (std::uint32_t peds = kMinPedCapacity; peds < kMaxPedCapacity; ++peds)
            monotonic = monotonic && WeaponCapacity(peds + 1) >= WeaponCapacity(peds);
        Check(monotonic, "capacity: the weapon registry never shrinks as the ped class grows");
        Check(PoolPedBudget(56) == 80 && PoolPedBudget(150) == 174, "capacity: 80 new pool handles per scan at 56, 174 at 150");

        // The views follow the session's value (and go back to the default).
        const PedCapacityValue pedView{};
        const LiveTargetsValue targetView{};
        const WeaponCapacityValue weaponView{};
        Check(VehicleCapacity() == 8, "capacity: the session's vehicle class is 8 until script.cpp sets it");
        Check(static_cast<std::uint32_t>(pedView) == 56 && static_cast<std::uint32_t>(targetView) == 64 &&
                static_cast<std::size_t>(weaponView) == 96,
            "capacity: the views read 56, 64 and 96 by default");
        SetPedCapacity(120);
        Check(static_cast<std::uint32_t>(pedView) == 120 && static_cast<std::uint32_t>(targetView) == 128 &&
                static_cast<std::uint32_t>(targetView) - static_cast<std::uint32_t>(pedView) == 8,
            "capacity: PedCapacity=120 gives 128 places, still 8 for vehicles");
        SetPedCapacity(150);
        Check(static_cast<std::uint32_t>(pedView) == 150 && static_cast<std::uint32_t>(targetView) == 158 &&
                static_cast<std::size_t>(weaponView) == 258,
            "capacity: PedCapacity=150 (GTA model) gives 158 places and 258 weapons");
        SetPedCapacity(1000);
        Check(PedCapacity() == 150, "capacity: SetPedCapacity clamps to the pool");
        // The GTA model's session: 148 peds and 79 vehicles.
        SetPedCapacity(DefaultPedCapacity(true));
        SetVehicleCapacity(VehicleCapacityFor(true));
        Check(VehicleCapacity() == 79 && static_cast<std::uint32_t>(targetView) == 227 &&
                static_cast<std::uint32_t>(targetView) <= kLiveTargetSlots,
            "capacity: the GTA model gives 227 places (148 peds, 79 vehicles)");
        SetVehicleCapacity(1000);
        Check(VehicleCapacity() == 79, "capacity: SetVehicleCapacity clamps to the pool less 1");
        SetVehicleCapacity(kLegacyVehicleCapacity);
        SetPedCapacity(kDefaultPedCapacity);

        // Late mask: every slot alone, and against the one-word reference.
        static SlotMask<kLiveTargetSlots> mask;
        Check(SlotMask<kLiveTargetSlots>::kWords == 6 && SlotMask<256>::kWords == 4 && SlotMask<128>::kWords == 2 &&
                SlotMask<64>::kWords == 1,
            "late mask: six words for 327 slots, four for 256, two for 128, one for 64");
        bool single = true;
        for (std::uint32_t slot = 0; slot < kLiveTargetSlots; ++slot)
        {
            mask.Set(slot, true);
            const std::vector<std::uint32_t> set = SetSlots(mask);
            single = single && set.size() == 1 && set[0] == slot;
            mask.Set(slot, false);
            single = single && SetSlots(mask).empty();
        }
        Check(single, "late mask: each slot sets and clears its own bit only");
        mask.Set(kLiveTargetSlots, true);
        Check(SetSlots(mask).empty(), "late mask: a slot past the end is ignored");
        std::mt19937 random(176001);
        std::uint64_t reference = 0;
        bool sameAsReference = true;
        std::vector<bool> expected(kLiveTargetSlots, false);
        bool sameSet = true;
        for (int step = 0; step < 20000; ++step)
        {
            const std::uint32_t slot = static_cast<std::uint32_t>(random() % kLiveTargetSlots);
            const bool late = (random() & 1u) != 0;
            mask.Set(slot, late);
            expected[slot] = late;
            if (slot < 64)
                reference = ReferenceLateWord(reference, slot, late);
            std::uint64_t words[SlotMask<kLiveTargetSlots>::kWords];
            mask.Load(words);
            sameAsReference = sameAsReference && words[0] == reference;
            if (step % 97 == 0)
            {
                std::vector<std::uint32_t> want;
                for (std::uint32_t index = 0; index < kLiveTargetSlots; ++index)
                    if (expected[index])
                        want.push_back(index);
                sameSet = sameSet && SetSlots(mask) == want;
            }
        }
        Check(sameAsReference, "late mask: slots 0..63 give exactly the reference word");
        Check(sameSet, "late mask: the iteration visits exactly the late slots, in slot order");
        mask.Clear();
        Check(SetSlots(mask).empty(), "late mask: Clear empties every word");

        // Scan end over a simulated session (Start takes the lowest free
        // slot among the places, Stop frees any, a full disarm resets it):
        // every armed slot is below the end, and at the default places the
        // end never passes 64 (the lookups scan what the fixed arrays did).
        for (const std::uint32_t places : {64u, 128u, 158u, 227u})
        {
            std::vector<bool> armed(kLiveTargetSlots, false);
            std::uint32_t scanEnd = 0;
            std::uint32_t maxEnd = 0;
            bool covered = true;
            for (int step = 0; step < 50000; ++step)
            {
                const std::uint32_t op = static_cast<std::uint32_t>(random() % 100u);
                if (op < 55)
                {
                    for (std::uint32_t slot = 0; slot < places; ++slot)
                    {
                        if (!armed[slot])
                        {
                            scanEnd = RaisedScanEnd(scanEnd, slot);
                            armed[slot] = true;
                            break;
                        }
                    }
                }
                else if (op < 99)
                {
                    armed[random() % places] = false;
                }
                else
                {
                    std::fill(armed.begin(), armed.end(), false);
                    scanEnd = 0;
                }
                for (std::uint32_t slot = 0; slot < kLiveTargetSlots; ++slot)
                    covered = covered && (!armed[slot] || slot < scanEnd);
                maxEnd = (std::max)(maxEnd, scanEnd);
            }
            Check(covered, "scan end: every armed slot is below it");
            Check(maxEnd <= places, "scan end: never past the session's places (64 by default)");
        }
    }

    // The selection over a town crowd like a recorded one (124 peds,
    // the 56th nearest at ~80 m): with 56 places the farthest recorded ped
    // is the 56th nearest; with 120 every ped inside the 150 m enter radius
    // is recorded. And the object places (object_pose.h) follow the class.
    void TestCapacityReach()
    {
        using namespace replay::entity;
        // Distances: a dense core (80 peds in 80 m) and a sparser ring (44
        // from 80 to 160 m), sorted by construction; handle 1 is the player.
        std::vector<Candidate> crowd;
        crowd.push_back({1, 0.0f, true});
        for (std::int32_t n = 0; n < 80; ++n)
            crowd.push_back({100 + n, 4.0f + 76.0f * static_cast<float>(n) / 79.0f});
        for (std::int32_t n = 0; n < 44; ++n)
            crowd.push_back({200 + n, 81.0f + 79.0f * static_cast<float>(n) / 43.0f});
        std::size_t insideEnter = 0;
        for (const Candidate& candidate : crowd)
            insideEnter += candidate.distance <= 150.0f ? 1 : 0;

        const auto reach = [&](std::uint32_t capacity, std::size_t& recorded, std::size_t& skipped) {
            SelectionConfig config;
            config.capacity = replay::object_pose::PedPlaces(capacity, 0);
            std::vector<std::int32_t> retire;
            std::vector<std::int32_t> add;
            SelectionReport report;
            SelectEntities(crowd, {}, config, retire, add, &report);
            recorded = add.size();
            skipped = report.skippedFull.size();
            float farthest = 0.0f;
            for (const std::int32_t handle : add)
            {
                for (const Candidate& candidate : crowd)
                    if (candidate.handle == handle)
                        farthest = (std::max)(farthest, candidate.distance);
            }
            return farthest;
        };
        std::size_t recorded56 = 0, skipped56 = 0, recorded120 = 0, skipped120 = 0;
        const float reach56 = reach(56, recorded56, skipped56);
        const float reach120 = reach(120, recorded120, skipped120);
        std::printf("Capacity reach (synthetic town, %zu peds inside 150 m): 56 places record %zu out to %.0f m "
                    "(%zu left out), 120 record %zu out to %.0f m (%zu left out)\n",
            insideEnter, recorded56, static_cast<double>(reach56), skipped56, recorded120,
            static_cast<double>(reach120), skipped120);
        Check(recorded56 == 56 && reach56 < 81.0f && skipped56 == insideEnter - 56,
            "reach: 56 places stop at the 56th nearest, the rest inside 150 m left out");
        Check(recorded120 == insideEnter && skipped120 == 0 && reach120 <= 150.0f && reach120 > 145.0f,
            "reach: 120 places record every ped inside the enter radius");

        // An object (the pelt) takes a place from the ped class: with 100
        // ped bodies it is refused at 56 and accepted at 120.
        Check(!replay::object_pose::ShouldPose(false, 26, 100, 0, 4, 56, 108, 64),
            "object places: no place with the class full at 56");
        Check(replay::object_pose::ShouldPose(false, 26, 100, 0, 4, 120, 108, 128),
            "object places: a place at PedCapacity=120 with 100 bodies");
    }

    // The vehicle class by the pool (clip 1171-Blackwater): 8 recorded
    // vehicles locked by draft horses and seats, 4 more inside the enter
    // radius (the nearest at 36 m). With 8 places the 4 were left out and
    // none could be evicted (a wagon's horses and driver recorded, the wagon
    // not); with the pool less 1 every one enters. A full pool (80, the
    // player's wagon pinned) records 79, the farthest left out.
    void TestVehiclePoolSelection()
    {
        using namespace replay::entity;
        std::vector<Candidate> street;
        std::vector<std::int32_t> recorded;
        for (std::int32_t n = 0; n < 8; ++n)
        {
            Candidate locked{500 + n, 5.0f + 4.0f * static_cast<float>(n)};
            locked.locked = true;
            street.push_back(locked);
            recorded.push_back(locked.handle);
        }
        for (const float distance : {36.0f, 48.0f, 60.0f, 70.0f})
            street.push_back({600 + static_cast<std::int32_t>(distance), distance});
        const auto select = [&](std::uint32_t capacity, const std::vector<Candidate>& candidates,
                                const std::vector<std::int32_t>& now, std::vector<std::int32_t>& add,
                                SelectionReport& report) {
            SelectionConfig config;
            config.capacity = capacity;
            std::vector<std::int32_t> retire;
            SelectEntities(candidates, now, config, retire, add, &report);
            return retire.size();
        };
        std::vector<std::int32_t> add;
        SelectionReport report;
        std::size_t retired = select(replay::capacity::VehicleCapacityFor(false), street, recorded, add, report);
        Check(add.empty() && retired == 0 && report.skippedFull.size() == 4 && report.evicted.empty() &&
                !report.skippedFull.empty() && report.skippedFull.front() == 636,
            "vehicle pool: 8 places, the 4 newcomers (nearest 36 m) left out, the locked 8 kept");
        add.clear();
        report = {};
        retired = select(replay::capacity::VehicleCapacityFor(true), street, recorded, add, report);
        Check(add.size() == 4 && retired == 0 && report.skippedFull.empty() && !add.empty() && add.front() == 636,
            "vehicle pool: with the pool less 1 every newcomer enters, nearest first");

        std::vector<Candidate> full;
        Candidate player{700, 0.0f, true};
        full.push_back(player);
        for (std::int32_t n = 1; n < static_cast<std::int32_t>(replay::capacity::kVehiclePoolSlots); ++n)
            full.push_back({700 + n, 1.5f * static_cast<float>(n)});
        add.clear();
        report = {};
        select(replay::capacity::VehicleCapacityFor(true), full, {}, add, report);
        Check(add.size() == replay::capacity::kMaxVehicleCapacity && report.skippedFull.size() == 1 &&
                report.skippedFull.front() == 779 && std::find(add.begin(), add.end(), 700) != add.end(),
            "vehicle pool: a full pool records 79 with the player's vehicle, the farthest left out");
    }
}

int RunPoseCaptureRulesTests()
{
    TestLateDecisionRule();
    TestLateSynthetic();
#ifdef _WIN32
    TestLateClips();
#endif
    BenchLateCheck();
    TestStallPollDue();
    TestStallPolls();
    TestStallPollDefault();
    TestLateBackoff();
    TestFirstCheckStagger();
    TestHeldFrames();
    TestDrainListRules();
    TestDrainListModel();
    TestBufferPrune();
    TestCaptureCapacity();
    TestCapacityReach();
    TestVehiclePoolSelection();
    if (g_rulesFailures == 0)
        std::puts("Pose capture rules tests passed.");
    return g_rulesFailures;
}
