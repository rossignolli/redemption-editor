// The playback side of the pose path, offline.
//
// R1 (sample reuse, replay_playback.cpp with playback_cache.h): a pose sample
// at the time of the track's previous sample is that sample again, bit for
// bit, and leaves the track as the previous call left it. Every track of the
// recorded clips (REPLAYPOC_POSE_CLIP_DIR) and synthetic tracks are played
// along an editor path with pauses (the playhead held for a few ticks during
// play, slow motion under 1 ms a tick, the start hold, the clip end, after
// seeks and backward scrubs, before the first frame and after the last)
// twice: the plain way (a sample every tick, published when it placed)
// and the R1 way (a sample only when the record, the playhead or the slot
// changed; otherwise the slot keeps what it holds and the tick reuses the
// placed transform and the root bone). Every tick the override slot's pose
// (locals, objects, both world matrices, t, frame times), whether the puppet
// placed, its world transform and the jerk detector's root bone must be the
// same bits.
//
// R3 (world clear, HidePoseV2World): HandleSet against the linear any_of it
// replaces, on random handle lists with inserts between queries.
//
// R4 (ped_blood.cpp Readable, readable_page.h): the answer of a page's
// working set entry against VirtualQuery's for ranges on this process's own
// pages of every kind (read-write touched and untouched, read-only,
// no-access, guard touched and untouched, reserved, freed, half no-access,
// code, image header, stack, the stack's guard page, heap), at offsets that
// stay in a page and that cross into the next; and the time of each on a
// 256 MB region whose pages are all resident.
//
// Benchmarks (printed, no pass or fail): the in-game order, one sample of
// every live track per tick at 45 fps (round robin over the puppets),
// against the same samples track after track; the same round robin with 1 MB
// of other memory touched between samples (what the natives between two
// puppets do to the caches, roughly); and what a paused tick cost per
// puppet without R1 (a repeated sample, round robin).

#include "pose_stream.h"

#include "clip_file.h"
#include "playback_cache.h"
#include "profiler.h"
#include "readable_page.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using namespace replay::pose;

    int g_playbackFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_playbackFailures;
    }

    // replay_playback.cpp SampleWorldTransform, verbatim.
    bool SampleWorld(const PoseSample& sample, WorldTransform& world)
    {
        WorldTransform a{};
        if (!DecodeWorldMatrix(sample.worldA, a))
            return false;
        WorldTransform b{};
        world = DecodeWorldMatrix(sample.worldB, b) ? InterpolateWorld(a, b, sample.t) : a;
        return true;
    }

    bool SameSample(const PoseSample& a, const PoseSample& b)
    {
        return a.t == b.t && a.frameTimeA == b.frameTimeA && a.frameTimeB == b.frameTimeB &&
            a.locals.size() == b.locals.size() && a.objects.size() == b.objects.size() &&
            std::memcmp(a.locals.data(), b.locals.data(), a.locals.size() * sizeof(BoneQts)) == 0 &&
            std::memcmp(a.objects.data(), b.objects.data(), a.objects.size() * sizeof(BoneQts)) == 0 &&
            std::memcmp(a.worldA, b.worldA, sizeof(a.worldA)) == 0 &&
            std::memcmp(a.worldB, b.worldB, sizeof(a.worldB)) == 0;
    }

    // ---------------------------------------------------------------------
    // R1

    // The editor's playhead with the pauses R1 is for: 45 fps play (22, 22,
    // 23 ms a tick) held now and then for 1 to 6 ticks, the start hold, the
    // end held, slow motion at a quarter of a millisecond a tick (each ms
    // sampled four times), seeks and backward scrubs each followed by a
    // pause, a jump before the first frame and past the last (clamped).
    std::vector<std::uint32_t> PausePath(std::uint32_t first, std::uint32_t last)
    {
        std::vector<std::uint32_t> times;
        const auto hold = [&](std::uint32_t time, int ticks) {
            for (int tick = 0; tick < ticks; ++tick)
                times.push_back(time);
        };
        std::mt19937 random(1760);
        const std::uint32_t span = last - first;
        const std::uint32_t playEnd = std::min(last, first + 60000u);
        hold(first, 4); // the start hold
        for (std::uint32_t time = first, tick = 0; time <= playEnd; time += (tick++ % 3 == 2) ? 23u : 22u)
        {
            times.push_back(time);
            if (random() % 29 == 0)
                hold(time, 1 + static_cast<int>(random() % 6));
        }
        hold(playEnd, 5);
        hold(last, 5); // held at the end
        const std::uint32_t slowFrom = first + span / 4;
        for (std::uint32_t quarter = 0; quarter < 4u * 600u; ++quarter)
            times.push_back(std::min(last, slowFrom + quarter / 4u)); // 0.25 ms a tick
        for (int seek = 0; seek < 24; ++seek)
        {
            const std::uint32_t at = first + static_cast<std::uint32_t>(random() % (span + 1u));
            hold(at, 3);
            const std::uint32_t to = std::min(last, at + 300u);
            for (std::uint32_t time = at; time <= to; time += 22u)
                times.push_back(time);
            hold(times.back(), 2);
            for (std::uint32_t time = to; time >= first + 33u && time + 400u >= to; time -= 33u)
                times.push_back(time); // scrub back
            hold(times.back(), 4);
        }
        hold(0u, 3);
        hold(last + 5000u, 3);
        hold(first, 2);
        return times;
    }

    struct ReuseComparison
    {
        std::size_t ticks = 0;
        std::size_t reused = 0;
        std::size_t sampled = 0;
        std::size_t differ = 0;
        std::size_t firstDiffer = ~static_cast<std::size_t>(0);
    };

    // One puppet over `times`, both ways (see the top of the file). The
    // record and the slot do not change here; the game's key adds them and
    // drops the reuse on any arm or release.
    ReuseComparison CompareReuseOverPath(const PoseTrack& pristine, const std::vector<std::uint32_t>& times)
    {
        constexpr std::uint32_t kRecord = 0x1234u;
        constexpr int kSlot = 3;
        ReuseComparison result;
        PoseTrack everyTick = pristine;
        PoseTrack reusing = pristine;
        PoseSample sampleA;
        PoseSample sampleB;
        PoseSample slotA; // the override slot of each side: the last sample published
        PoseSample slotB;
        bool slotAHas = false;
        bool slotBHas = false;
        replay::playback::SampleReuse reuse;
        for (std::size_t index = 0; index < times.size(); ++index)
        {
            const std::uint32_t time = times[index];
            // Plain: sample, place, publish (swap) every tick.
            WorldTransform worldA{};
            const bool placedA = everyTick.Sample(time, sampleA) && SampleWorld(sampleA, worldA);
            float rootA[3] = {};
            const bool hasRootA = placedA && !sampleA.objects.empty();
            if (hasRootA)
                std::memcpy(rootA, sampleA.objects[0].translation, sizeof(rootA));
            if (placedA)
            {
                std::swap(slotA, sampleA);
                slotAHas = true;
            }
            // R1.
            WorldTransform worldB{};
            bool placedB = false;
            float rootB[3] = {};
            bool hasRootB = false;
            if (reuse.Matches(kRecord, time, kSlot))
            {
                ++result.reused;
                placedB = reuse.sampled;
                worldB = reuse.world;
                hasRootB = reuse.hasRoot;
                std::memcpy(rootB, reuse.root, sizeof(rootB));
            }
            else
            {
                ++result.sampled;
                placedB = reusing.Sample(time, sampleB) && SampleWorld(sampleB, worldB);
                hasRootB = placedB && !sampleB.objects.empty();
                if (hasRootB)
                    std::memcpy(rootB, sampleB.objects[0].translation, sizeof(rootB));
                reuse.Begin(kRecord, time, kSlot, placedB, worldB, hasRootB ? rootB : nullptr);
                if (placedB)
                {
                    std::swap(slotB, sampleB);
                    slotBHas = true;
                }
                reuse.Keep(); // the publish took it (the game: no alternate, not a vehicle)
            }
            ++result.ticks;
            const bool same = placedA == placedB && slotAHas == slotBHas &&
                (!slotAHas || SameSample(slotA, slotB)) &&
                (!placedA || std::memcmp(&worldA, &worldB, sizeof(worldA)) == 0) &&
                hasRootA == hasRootB && (!hasRootA || std::memcmp(rootA, rootB, sizeof(rootA)) == 0);
            if (!same)
            {
                if (result.differ == 0)
                    result.firstDiffer = index;
                ++result.differ;
            }
        }
        return result;
    }

    // A small synthetic track (runs without the clip folder): a chain of
    // bones with a body that moves every frame, a face that moves now and
    // then, no-interpolation frames and a layout change.
    PoseTrack MakeSyntheticTrack(std::uint32_t bones, std::uint32_t frames)
    {
        SkeletonLayout layout;
        layout.boneCount = bones;
        layout.order = CompositionOrder::ParentThenLocal;
        layout.parent.assign(bones, kRootParent);
        layout.ambiguous.assign(bones, 0);
        layout.highPrecision.assign(bones, 0);
        for (std::uint32_t bone = 1; bone < bones; ++bone)
            layout.parent[bone] = static_cast<std::int32_t>(bone < 8 ? bone - 1 : (bone % 8));
        layout.evaluationOrder.clear();
        for (std::uint32_t bone = 0; bone < bones; ++bone)
            layout.evaluationOrder.push_back(bone);
        StreamConfig config;
        PoseTrack track;
        track.Begin(layout, config);
        SkeletonLayout changed = layout;
        changed.parent[bones / 2] = kUnresolvedParent;
        changed.unresolvedBones.assign(1, bones / 2);
        std::vector<BoneQts> locals(bones);
        std::vector<BoneQts> objects(bones);
        float world[16] = {};
        world[0] = 1.0f;
        world[5] = 1.0f;
        world[10] = 1.0f;
        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
            if (frame == frames * 3 / 5)
                track.UpdateLayout(changed);
            for (std::uint32_t bone = 0; bone < bones; ++bone)
            {
                BoneQts& local = locals[bone];
                const bool moves = bone < 8 || (bone % 5 == 0 && frame % 7 == 0);
                const float phase = moves ? 0.05f * static_cast<float>(frame) + 0.1f * static_cast<float>(bone) : 0.0f;
                local.rotation = Normalize({0.1f * std::sin(phase), 0.05f * std::cos(phase), 0.0f, 1.0f});
                local.translation[0] = 0.02f * static_cast<float>(bone % 9);
                local.translation[1] = 0.01f * std::sin(phase);
                local.translation[2] = 0.03f;
                local.translation[3] = 0.0f;
                for (int axis = 0; axis < 4; ++axis)
                    local.scale[axis] = 1.0f;
            }
            for (std::uint32_t bone = 0; bone < bones; ++bone)
            {
                const std::int32_t parent = layout.parent[bone];
                objects[bone] = parent >= 0 ? Compose(objects[static_cast<std::uint32_t>(parent)], locals[bone],
                                                  layout.order, layout.inheritScale)
                                            : locals[bone];
            }
            world[12] = 0.04f * static_cast<float>(frame) + (frame == frames / 3 ? 5.0f : 0.0f);
            FrameInput input;
            input.timeMs = frame * 33u + (frame % 4 == 1 ? 1u : 0u);
            input.flags = (frame % 53 == 17) ? kFrameNoInterpolation : 0;
            input.world = world;
            input.locals = locals.data();
            input.objects = objects.data();
            track.Append(input);
        }
        return track;
    }

    void TestSampleReuseKey()
    {
        replay::playback::SampleReuse reuse;
        Check(!reuse.Matches(0, 0, -1), "R1: a new puppet has no reuse");
        const WorldTransform placed{{1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
        const float root[3] = {0.5f, 0.25f, 0.125f};
        reuse.Begin(7, 100, 2, true, placed, root);
        Check(!reuse.Matches(7, 100, 2), "R1: a sample is not reused before the tick kept it");
        reuse.Keep();
        Check(reuse.Matches(7, 100, 2), "R1: same record, playhead and slot reuse the sample");
        Check(!reuse.Matches(8, 100, 2) && !reuse.Matches(7, 101, 2) && !reuse.Matches(7, 100, 3) &&
                !reuse.Matches(7, 100, -1),
            "R1: another record, playhead or slot samples again");
        Check(reuse.hasRoot && reuse.root[2] == 0.125f && reuse.world.position.y == 2.0f && reuse.sampled,
            "R1: the reuse keeps the placed transform and the root bone");
        reuse.Invalidate();
        Check(!reuse.Matches(7, 100, 2), "R1: an arm or a release drops the reuse");
        reuse.Begin(7, 100, -1, false, placed, nullptr);
        reuse.Keep();
        Check(reuse.Matches(7, 100, -1) && !reuse.sampled && !reuse.hasRoot,
            "R1: an unarmed puppet reuses the transform only (and a failed sample stays failed)");
    }

    void TestSampleReuseSynthetic()
    {
        std::size_t ticks = 0;
        std::size_t reused = 0;
        std::size_t differ = 0;
        for (const std::uint32_t bones : {40u, 300u})
        {
            const PoseTrack track = MakeSyntheticTrack(bones, 400); // 13 s, 7 chunks, a layout change
            const ReuseComparison result =
                CompareReuseOverPath(track, PausePath(track.Stats().firstTimeMs, track.Stats().lastTimeMs));
            ticks += result.ticks;
            reused += result.reused;
            differ += result.differ;
        }
        std::printf("  R1 synthetic tracks: %zu ticks, %zu reused, %zu differ\n", ticks, reused, differ);
        Check(differ == 0 && reused > 0, "R1: reusing the sample on a held playhead publishes and places the same bits (synthetic)");
    }

    // ---------------------------------------------------------------------
    // Recorded clips.

    struct LoadedClip
    {
        std::wstring name;
        std::vector<PoseRecordExport> poses;
    };

    std::vector<LoadedClip> LoadClips()
    {
        std::vector<LoadedClip> clips;
        wchar_t folder[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"REPLAYPOC_POSE_CLIP_DIR", folder, MAX_PATH))
            return clips;
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ignored))
        {
            if (entry.path().extension() != L".rpclip")
                continue;
            replay::Recording recording;
            replay::camera::Track camera;
            std::uint64_t duration = 0;
            std::string error;
            LoadedClip clip;
            clip.name = entry.path().filename().wstring();
            if (!replay::clipfile::Load(entry.path().wstring(), 0x140000000ull, 0x8000000, recording, clip.poses, camera,
                    duration, error))
            {
                std::fprintf(stderr, "  playback clip %ls: not loaded (%s)\n", clip.name.c_str(), error.c_str());
                continue;
            }
            clips.push_back(std::move(clip));
        }
        return clips;
    }

    void TestSampleReuseClips(const std::vector<LoadedClip>& clips)
    {
        for (const LoadedClip& clip : clips)
        {
            const auto start = std::chrono::steady_clock::now();
            std::size_t tracks = 0;
            ReuseComparison total;
            for (const PoseRecordExport& record : clip.poses)
            {
                PoseTrack track;
                std::string importError;
                if (record.track.chunks.empty() || !track.Import(record.track, importError))
                    continue;
                ++tracks;
                const ReuseComparison result =
                    CompareReuseOverPath(track, PausePath(track.Stats().firstTimeMs, track.Stats().lastTimeMs));
                total.ticks += result.ticks;
                total.reused += result.reused;
                total.sampled += result.sampled;
                total.differ += result.differ;
                if (result.differ)
                {
                    std::fprintf(stderr, "  R1 clip %ls: record 0x%x (%u bones) differs at tick %zu\n", clip.name.c_str(),
                        record.recordId, record.boneCount, result.firstDiffer);
                }
            }
            std::printf("  R1 clip %ls: %zu tracks, %zu ticks (%zu sampled, %zu reused), %zu differ (%.1f s)\n",
                clip.name.c_str(), tracks, total.ticks, total.sampled, total.reused, total.differ,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            Check(total.differ == 0 && total.reused > 0,
                "R1: reusing the sample on a held playhead publishes and places the same bits (recorded clip)");
        }
    }

    // ---------------------------------------------------------------------
    // R3

    void TestHandleSet()
    {
        std::mt19937 random(176);
        std::size_t queries = 0;
        std::size_t differ = 0;
        for (int round = 0; round < 200; ++round)
        {
            struct Item
            {
                std::int32_t handle;
            };
            std::vector<Item> list;
            const int size = static_cast<int>(random() % 300);
            for (int n = 0; n < size; ++n)
                list.push_back({static_cast<std::int32_t>(random() % 2000) - 50}); // duplicates and 0 too
            replay::playback::HandleSet set;
            set.Assign(list, [](const Item& item) { return item.handle; });
            for (int query = 0; query < 400; ++query)
            {
                if (query % 50 == 49)
                {
                    const std::int32_t added = static_cast<std::int32_t>(random() % 2000) - 50;
                    list.push_back({added}); // the hidden world grows during a pass
                    set.Add(added);
                }
                const std::int32_t handle = static_cast<std::int32_t>(random() % 2100) - 75;
                const bool linear = std::any_of(list.begin(), list.end(), [&](const Item& item) { return item.handle == handle; });
                ++queries;
                differ += linear != set.Contains(handle) ? 1u : 0u;
            }
        }
        std::printf("  R3 world clear lookups: %zu queries against any_of, %zu differ\n", queries, differ);
        Check(differ == 0, "R3: the sorted handle set answers what any_of over the list answered");
    }

    // ---------------------------------------------------------------------
    // R4

    struct ReadableTally
    {
        std::size_t ranges = 0;
        std::size_t answered = 0;
        std::size_t answeredReadable = 0;
        std::size_t differ = 0;
    };

    // Both answers for [address, address + bytes): the working set entry's
    // must be VirtualQuery's whenever it answers.
    void CompareReadable(const std::uint8_t* address, std::size_t bytes, ReadableTally& tally)
    {
        bool answered = false;
        const bool byPage = replay::mem::ReadableByWorkingSet(address, bytes, answered);
        std::uintptr_t base = 0;
        std::uintptr_t end = 0;
        bool queried = false;
        const bool byQuery = replay::mem::ReadableByQuery(address, bytes, base, end, queried);
        ++tally.ranges;
        if (!answered)
            return;
        ++tally.answered;
        tally.answeredReadable += byPage ? 1u : 0u;
        if (byPage != byQuery)
        {
            ++tally.differ;
            std::fprintf(stderr, "  R4: %p + %zu: page entry says %d, VirtualQuery %d\n", static_cast<const void*>(address),
                bytes, byPage ? 1 : 0, byQuery ? 1 : 0);
        }
    }

    void CompareRegion(const std::uint8_t* base, std::size_t page, std::size_t pages, ReadableTally& tally)
    {
        static const std::size_t kSizes[] = {1, 4, 0x20, 0x100, 0};
        for (std::size_t index = 0; index < pages; ++index)
        {
            const std::uint8_t* at = base + index * page;
            for (const std::size_t offset : {std::size_t{0}, std::size_t{8}, std::size_t{0x9C}, page / 2, page - 0x20,
                     page - 4, page - 2})
            {
                for (const std::size_t bytes : kSizes)
                    CompareReadable(at + offset, bytes, tally); // page - 2 with 4 bytes and up crosses
                CompareReadable(at + offset, page, tally);
            }
        }
    }

    void TestReadablePage()
    {
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const std::size_t page = system.dwPageSize;
        Check(page == replay::mem::kPageBytes, "R4: the page size is 4 KB");
        ReadableTally tally;
        ReadableTally guardTally;
        const auto alloc = [&](std::size_t pages, DWORD type, DWORD protect) {
            return static_cast<std::uint8_t*>(VirtualAlloc(nullptr, pages * page, type, protect));
        };
        // Read-write, two pages touched (in the working set), two not.
        std::uint8_t* readWrite = alloc(4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        // Read-only, touched before the protection changed.
        std::uint8_t* readOnly = alloc(2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        std::uint8_t* noAccess = alloc(2, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
        // Guard pages, the first touched before the guard went on.
        std::uint8_t* guard = alloc(2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        std::uint8_t* reserved = alloc(2, MEM_RESERVE, PAGE_NOACCESS);
        std::uint8_t* freed = alloc(2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        // A read-write page followed by a no-access one (ranges across them).
        std::uint8_t* mixed = alloc(2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!readWrite || !readOnly || !noAccess || !guard || !reserved || !freed || !mixed)
        {
            Check(false, "R4: test pages allocated");
            return;
        }
        DWORD old = 0;
        readWrite[0] = 1;
        readWrite[page] = 1;
        readOnly[0] = 1;
        readOnly[page] = 1;
        VirtualProtect(readOnly, 2 * page, PAGE_READONLY, &old);
        guard[0] = 1;
        VirtualProtect(guard, 2 * page, PAGE_READWRITE | PAGE_GUARD, &old);
        freed[0] = 1;
        freed[page] = 1;
        VirtualFree(freed, 0, MEM_RELEASE);
        mixed[0] = 1;
        mixed[page] = 1;
        VirtualProtect(mixed + page, page, PAGE_NOACCESS, &old);
        CompareRegion(readWrite, page, 4, tally);
        CompareRegion(readOnly, page, 2, tally);
        CompareRegion(noAccess, page, 2, tally);
        CompareRegion(guard, page, 2, guardTally);
        CompareRegion(reserved, page, 2, tally);
        CompareRegion(freed, page, 2, tally);
        CompareRegion(mixed, page, 2, tally);
        // Code, the image header, the stack and its guard page, the heap.
        const auto* code = reinterpret_cast<const std::uint8_t*>(&TestReadablePage);
        CompareRegion(reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(code) & ~(page - 1)), page, 1,
            tally);
        CompareRegion(reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr)), page, 1, tally);
        volatile std::uint8_t onStack[64] = {};
        onStack[0] = 1;
        CompareRegion(reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(&onStack[0]) & ~(page - 1)),
            page, 1, tally);
        const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        CompareRegion(static_cast<const std::uint8_t*>(tib->StackLimit) - page, page, 1, guardTally);
        std::vector<std::uint8_t> heap(3 * page, 1);
        CompareRegion(heap.data(), page, 2, tally);
        CompareReadable(nullptr, 4, tally);
        std::printf("  R4 readability by page entry: %zu ranges, %zu answered by the page (%zu readable), %zu differ from "
                    "VirtualQuery; guard pages: %zu ranges, %zu answered (%zu readable), %zu differ\n",
            tally.ranges, tally.answered, tally.answeredReadable, tally.differ, guardTally.ranges, guardTally.answered,
            guardTally.answeredReadable, guardTally.differ);
        Check(tally.differ == 0 && guardTally.differ == 0 && guardTally.answeredReadable == 0,
            "R4: the page entry's answer is VirtualQuery's (never readable on a guard page)");
        Check(tally.answered > 0 && tally.answeredReadable > 0, "R4: resident pages are answered by their entry");
        VirtualFree(readWrite, 0, MEM_RELEASE);
        VirtualProtect(readOnly, 2 * page, PAGE_READWRITE, &old);
        VirtualFree(readOnly, 0, MEM_RELEASE);
        VirtualFree(noAccess, 0, MEM_RELEASE);
        VirtualFree(guard, 0, MEM_RELEASE);
        VirtualFree(reserved, 0, MEM_RELEASE);
        VirtualFree(mixed, 0, MEM_RELEASE);

        // The time of each answer on a 256 MB region of resident pages (a
        // pool of the engine is such a region): VirtualQuery walks it to
        // report its extent, the page entry does not.
        constexpr std::size_t kBigPages = 65536;
        std::uint8_t* big = alloc(kBigPages, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!big)
            return;
        for (std::size_t index = 0; index < kBigPages; ++index)
            big[index * page] = static_cast<std::uint8_t>(index);
        const std::uint8_t* probe = big + (kBigPages / 2) * page + 0x9C;
        constexpr int kRounds = 20;
        std::size_t sink = 0;
        auto start = std::chrono::steady_clock::now();
        for (int round = 0; round < kRounds; ++round)
        {
            std::uintptr_t base = 0;
            std::uintptr_t end = 0;
            bool queried = false;
            sink += replay::mem::ReadableByQuery(probe, 4, base, end, queried) ? 1u : 0u;
        }
        const double queryUs =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kRounds;
        start = std::chrono::steady_clock::now();
        for (int round = 0; round < kRounds; ++round)
        {
            bool answered = false;
            sink += replay::mem::ReadableByWorkingSet(probe, 4, answered) && answered ? 1u : 0u;
        }
        const double pageUs =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kRounds;
        std::printf("  R4 on a 256 MB region of resident pages: VirtualQuery %.1f us, page entry %.2f us per answer (sink %zu)\n",
            queryUs, pageUs, sink);
        VirtualFree(big, 0, MEM_RELEASE);
    }

    // ---------------------------------------------------------------------
    // Benchmarks.

    struct ClipSpan
    {
        std::uint32_t first = 0;
        std::uint32_t last = 0;
        std::uint32_t bones = 0;
    };

    std::vector<std::uint32_t> Ticks45(std::uint32_t from, std::uint32_t to)
    {
        std::vector<std::uint32_t> ticks;
        for (std::uint32_t time = from, tick = 0; time <= to; time += (tick++ % 3 == 2) ? 23u : 22u)
            ticks.push_back(time);
        return ticks;
    }

    // Touches `bytes` of other memory (read and write), as the natives
    // between two puppets do; a rough stand-in, nothing more.
    std::uint64_t TouchOther(std::vector<std::uint64_t>& other)
    {
        std::uint64_t sum = 0;
        for (std::size_t index = 0; index < other.size(); index += 8)
        {
            sum += other[index];
            other[index] = sum;
        }
        return sum;
    }

    double SamplesUs(const std::chrono::steady_clock::time_point& start, std::size_t samples)
    {
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        return samples ? us / static_cast<double>(samples) : 0.0;
    }

    void BenchmarkClip(const LoadedClip& clip)
    {
        std::vector<PoseTrack> pristine;
        std::vector<ClipSpan> spans;
        for (const PoseRecordExport& record : clip.poses)
        {
            PoseTrack track;
            std::string importError;
            if (record.track.chunks.empty() || !track.Import(record.track, importError))
                continue;
            spans.push_back({track.Stats().firstTimeMs, track.Stats().lastTimeMs, record.boneCount});
            pristine.push_back(std::move(track));
        }
        if (pristine.empty())
            return;
        std::uint32_t from = spans.front().first;
        std::uint32_t to = spans.front().last;
        for (const ClipSpan& span : spans)
        {
            from = std::min(from, span.first);
            to = std::max(to, span.last);
        }
        to = std::min(to, from + 30000u); // the first 30 s
        const std::vector<std::uint32_t> ticks = Ticks45(from, to);
        const auto live = [&](std::size_t track, std::uint32_t time) {
            return spans[track].first <= time && time <= spans[track].last;
        };
        PoseSample out;
        std::vector<std::uint64_t> other(std::size_t{1} << 17); // 1 MB
        std::uint64_t sink = 0;
        double interleavedUs = 1e30;
        double sequentialUs = 1e30;
        double pollutedUs = 1e30;
        double repeatedUs = 1e30;
        std::size_t samples = 0;
        std::size_t bonesSampled = 0;
        std::size_t peakLive = 0;
        for (int round = 0; round < 2; ++round)
        {
            // The game's order: every live puppet once per tick.
            std::vector<PoseTrack> tracks = pristine;
            samples = 0;
            bonesSampled = 0;
            auto start = std::chrono::steady_clock::now();
            for (const std::uint32_t time : ticks)
            {
                std::size_t liveNow = 0;
                for (std::size_t track = 0; track < tracks.size(); ++track)
                {
                    if (!live(track, time))
                        continue;
                    tracks[track].Sample(time, out);
                    ++samples;
                    ++liveNow;
                    bonesSampled += spans[track].bones;
                }
                peakLive = std::max(peakLive, liveNow);
            }
            interleavedUs = std::min(interleavedUs, SamplesUs(start, samples));
            // The same samples, track after track.
            tracks = pristine;
            start = std::chrono::steady_clock::now();
            for (std::size_t track = 0; track < tracks.size(); ++track)
            {
                for (const std::uint32_t time : ticks)
                {
                    if (live(track, time))
                        tracks[track].Sample(time, out);
                }
            }
            sequentialUs = std::min(sequentialUs, SamplesUs(start, samples));
            // Round robin with other memory touched between two puppets
            // (the touch timed apart and taken out).
            tracks = pristine;
            double touchUs = 0.0;
            start = std::chrono::steady_clock::now();
            for (const std::uint32_t time : ticks)
            {
                for (std::size_t track = 0; track < tracks.size(); ++track)
                {
                    if (!live(track, time))
                        continue;
                    const auto touchStart = std::chrono::steady_clock::now();
                    sink += TouchOther(other);
                    touchUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - touchStart).count();
                    tracks[track].Sample(time, out);
                }
            }
            const double pollutedTotal =
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() - touchUs;
            pollutedUs = std::min(pollutedUs, samples ? pollutedTotal / static_cast<double>(samples) : 0.0);
            // A paused tick without R1: every live puppet sampled again at the

            // time it was just sampled at, round robin, at 40 pause points.
            tracks = pristine;
            std::size_t repeats = 0;
            double repeatTotal = 0.0;
            for (std::size_t point = 0; point < 40; ++point)
            {
                const std::uint32_t time = ticks[point * ticks.size() / 40];
                for (std::size_t track = 0; track < tracks.size(); ++track)
                {
                    if (live(track, time))
                        tracks[track].Sample(time, out); // the tick that paused
                }
                const auto repeatStart = std::chrono::steady_clock::now();
                for (std::size_t track = 0; track < tracks.size(); ++track)
                {
                    if (!live(track, time))
                        continue;
                    tracks[track].Sample(time, out);
                    ++repeats;
                }
                repeatTotal += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - repeatStart).count();
            }
            repeatedUs = std::min(repeatedUs, repeats ? repeatTotal / static_cast<double>(repeats) : 0.0);
        }
        std::printf("  playback bench %ls: %zu tracks, %zu ticks at 45 fps (30 s), %zu samples (%.0f bones avg, up to %zu "
                    "puppets a tick); us per sample: round robin %.2f, track after track %.2f (x%.2f), round robin with 1 MB "
                    "touched between puppets %.2f; a paused tick's repeated sample %.2f (sink %llu)\n",
            clip.name.c_str(), pristine.size(), ticks.size(), samples,
            samples ? static_cast<double>(bonesSampled) / static_cast<double>(samples) : 0.0, peakLive, interleavedUs,
            sequentialUs, interleavedUs / std::max(sequentialUs, 1e-9), pollutedUs, repeatedUs,
            static_cast<unsigned long long>(sink & 1u));
    }

    // The game's publish: the shared sample buffer is swapped into the
    // puppet's override slot (PublishOverrideSample) and gets that slot's
    // previous buffer, which the engine's job threads copied into the
    // skeleton during the last frame (the detour, under the slot's lock).
    // So every sample writes 2 x bones x 48 bytes into lines another core
    // has just read. The round robin with that rotation, alone and with a
    // thread that copies every slot once after each tick, as the finalizer
    // calls do, timing the samples only.
    struct EmulatedSlot
    {
        SRWLOCK lock = SRWLOCK_INIT;
        std::vector<BoneQts> locals;
        std::vector<BoneQts> objects;
    };

    double PublishedRoundRobinUs(const std::vector<PoseTrack>& pristine, const std::vector<ClipSpan>& spans,
        const std::vector<std::uint32_t>& ticks, bool reader)
    {
        std::vector<PoseTrack> tracks = pristine;
        std::vector<EmulatedSlot> slots(tracks.size());
        PoseSample sample;
        std::atomic<std::uint64_t> published{0};
        std::atomic<std::uint64_t> copied{0};
        std::atomic<bool> stop{false};
        std::thread copier;
        if (reader)
        {
            copier = std::thread([&]() {
                std::vector<BoneQts> scratch;
                std::uint64_t done = 0;
                while (!stop.load(std::memory_order_acquire))
                {
                    if (published.load(std::memory_order_acquire) == done)
                    {
                        std::this_thread::yield();
                        continue;
                    }
                    for (EmulatedSlot& slot : slots)
                    {
                        AcquireSRWLockShared(&slot.lock);
                        const std::size_t locals = slot.locals.size();
                        const std::size_t objects = slot.objects.size();
                        if (scratch.size() < locals + objects)
                            scratch.resize(locals + objects);
                        if (locals)
                            std::memcpy(scratch.data(), slot.locals.data(), locals * sizeof(BoneQts));
                        if (objects)
                            std::memcpy(scratch.data() + locals, slot.objects.data(), objects * sizeof(BoneQts));
                        ReleaseSRWLockShared(&slot.lock);
                    }
                    ++done;
                    copied.store(done, std::memory_order_release);
                }
            });
        }
        std::size_t samples = 0;
        double us = 0.0;
        std::uint64_t tick = 0;
        for (const std::uint32_t time : ticks)
        {
            const auto start = std::chrono::steady_clock::now();
            for (std::size_t track = 0; track < tracks.size(); ++track)
            {
                if (spans[track].first > time || time > spans[track].last)
                    continue;
                tracks[track].Sample(time, sample);
                ++samples;
                EmulatedSlot& slot = slots[track];
                AcquireSRWLockExclusive(&slot.lock);
                slot.locals.swap(sample.locals);
                slot.objects.swap(sample.objects);
                ReleaseSRWLockExclusive(&slot.lock);
            }
            us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            ++tick;
            if (reader)
            {
                published.store(tick, std::memory_order_release);
                while (copied.load(std::memory_order_acquire) != tick)
                    std::this_thread::yield();
            }
        }
        if (reader)
        {
            stop.store(true, std::memory_order_release);
            copier.join();
        }
        return samples ? us / static_cast<double>(samples) : 0.0;
    }

    void BenchmarkPublishedClip(const LoadedClip& clip)
    {
        std::vector<PoseTrack> pristine;
        std::vector<ClipSpan> spans;
        for (const PoseRecordExport& record : clip.poses)
        {
            PoseTrack track;
            std::string importError;
            if (record.track.chunks.empty() || !track.Import(record.track, importError))
                continue;
            spans.push_back({track.Stats().firstTimeMs, track.Stats().lastTimeMs, record.boneCount});
            pristine.push_back(std::move(track));
        }
        if (pristine.empty())
            return;
        std::uint32_t from = spans.front().first;
        std::uint32_t to = spans.front().last;
        for (const ClipSpan& span : spans)
        {
            from = std::min(from, span.first);
            to = std::max(to, span.last);
        }
        to = std::min(to, from + 30000u);
        const std::vector<std::uint32_t> ticks = Ticks45(from, to);
        double alone = 1e30;
        double read = 1e30;
        for (int round = 0; round < 2; ++round)
        {
            alone = std::min(alone, PublishedRoundRobinUs(pristine, spans, ticks, false));
            read = std::min(read, PublishedRoundRobinUs(pristine, spans, ticks, true));
        }
        std::printf("    with the game's publish (shared sample buffer swapped into each puppet's slot): %.2f us per sample; "
                    "with another thread copying every slot after each tick, as the finalizer calls do: %.2f (x%.2f)\n",
            alone, read, read / std::max(alone, 1e-9));
    }

    // Where the round robin's time goes (the sampler's own zones) and the
    // work per pair at 45 fps.
    void SplitClip(const LoadedClip& clip)
    {
        // The profiler tests set a synthetic frequency (1 tick = 1 us); the
        // zones here are timed with the real counter.
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        replay::prof::SetTicksPerSecondForTests(static_cast<std::uint64_t>(frequency.QuadPart));
        std::vector<PoseTrack> tracks;
        std::vector<ClipSpan> spans;
        for (const PoseRecordExport& record : clip.poses)
        {
            PoseTrack track;
            std::string importError;
            if (record.track.chunks.empty() || !track.Import(record.track, importError))
                continue;
            spans.push_back({track.Stats().firstTimeMs, track.Stats().lastTimeMs, record.boneCount});
            tracks.push_back(std::move(track));
        }
        if (tracks.empty())
            return;
        std::uint32_t from = spans.front().first;
        std::uint32_t to = spans.front().last;
        for (const ClipSpan& span : spans)
        {
            from = std::min(from, span.first);
            to = std::max(to, span.last);
        }
        to = std::min(to, from + 30000u);
        PoseSample out;
        replay::prof::ResetSession();
        const SampleWork before = SampleWorkCounters();
        for (const std::uint32_t time : Ticks45(from, to))
        {
            for (std::size_t track = 0; track < tracks.size(); ++track)
            {
                if (spans[track].first <= time && time <= spans[track].last)
                    tracks[track].Sample(time, out);
            }
        }
        const SampleWork work = SampleWorkCounters();
        const replay::prof::Snapshot session = replay::prof::Session();
        std::printf("    split at 45 fps, round robin:");
        for (const replay::prof::ZoneStats& zone : session.zones)
        {
            if (std::strncmp(zone.name, "play.pose.", 10) == 0 && zone.calls > 0)
                std::printf(" %s %.2f us/call (%llu calls)", zone.name + 10, zone.avgUs,
                    static_cast<unsigned long long>(zone.calls));
        }
        const double pairs = static_cast<double>(std::max<std::uint64_t>(1, work.pairs - before.pairs));
        const double pairBones = static_cast<double>(std::max<std::uint64_t>(1, work.pairBones - before.pairBones));
        std::printf("\n    pairs: %.0f%% from A plus one record, bones reached %.0f%%, interpolated locals %.0f%% objects %.0f%%\n",
            100.0 * static_cast<double>(work.incrementalPairs - before.incrementalPairs) / pairs,
            100.0 * static_cast<double>(work.reachedBones - before.reachedBones) / pairBones,
            100.0 * static_cast<double>(work.blendLocals - before.blendLocals) / pairBones,
            100.0 * static_cast<double>(work.blendObjects - before.blendObjects) / pairBones);
        replay::prof::ResetSession();
    }
}

int RunPlaybackPerfTests()
{
    g_playbackFailures = 0;
    TestSampleReuseKey();
    TestSampleReuseSynthetic();
    TestHandleSet();
    TestReadablePage();
    const std::vector<LoadedClip> clips = LoadClips();
    TestSampleReuseClips(clips);
    for (const LoadedClip& clip : clips)
    {
        BenchmarkClip(clip);
        BenchmarkPublishedClip(clip);
        SplitClip(clip);
    }
    if (g_playbackFailures == 0)
        std::printf("Playback perf tests passed.\n");
    return g_playbackFailures;
}
