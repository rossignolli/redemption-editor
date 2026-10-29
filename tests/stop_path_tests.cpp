// The recording stop's tick without its long parts.
//
// Clip pointer audit: a single-threaded scan (blob order, one VirtualQuery
// per run of pages through RegionCache) is kept here as the reference; the
// split (ptraudit::Input on the script thread, ptraudit::Classify on a
// worker, in ascending address order) must write the same blobs= and
// pointers= text on a fake address space whose query answers like
// VirtualQuery (from the queried page to the end of its run), with no more
// queries and no more pages walked.
// Deferred log: the jobs' text in the order they were asked for, whole
// lines, the per-call byte budget, the quiet state of the tick that asked,
// and a failed job.
#include "clip_pointer_audit.h"
#include "deferred_log.h"
#include "log_policy.h"
#include "region_cache.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    int g_stopPathFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_stopPathFailures;
    }

    // A fake address space: runs of pages of one state. The query answers
    // like VirtualQuery: from the queried page to the end of its run, and
    // counts the pages it walked (the kernel's cost).
    struct FakeSpace
    {
        struct Run
        {
            std::uintptr_t start = 0;
            std::uintptr_t end = 0;
            bool committed = false;
        };
        std::vector<Run> runs; // ascending, contiguous
        std::uint64_t queries = 0;
        std::uint64_t pagesWalked = 0;

        bool Query(std::uintptr_t address, std::uintptr_t& start, std::uintptr_t& end, bool& committed)
        {
            ++queries;
            for (const Run& run : runs)
            {
                if (address >= run.start && address < run.end)
                {
                    start = address & ~static_cast<std::uintptr_t>(0xFFF);
                    end = run.end;
                    committed = run.committed;
                    pagesWalked += (end - start) / 0x1000;
                    return true;
                }
            }
            return false;
        }
    };

    // Blobs of the clip, as the stop scans them (kind, bytes).
    struct Blob
    {
        const char* kind = "";
        std::vector<std::uint8_t> bytes;
    };

    namespace v175
    {
        // The reference single-threaded scan and its text, with the query
        // injected.
        template <typename Query>
        std::string Audit(const std::vector<Blob>& clip, std::uintptr_t imageBase, std::uintptr_t imageEnd, Query&& queryRegion,
            std::uint32_t& queries)
        {
            replay::mem::RegionCache regions;
            struct Hits
            {
                std::uint32_t image = 0;
                std::uint32_t other = 0;
            };
            std::map<std::pair<std::string, std::size_t>, Hits> found;
            std::map<std::string, std::size_t> blobs;
            const auto scan = [&](const char* kind, const std::uint8_t* data, std::size_t bytes) {
                ++blobs[kind];
                for (std::size_t offset = 0; offset + 8 <= bytes; offset += 8)
                {
                    std::uint64_t value = 0;
                    std::memcpy(&value, data + offset, sizeof(value));
                    if (value < 0x10000 || value > 0x7FFFFFFEFFFFull || (value & 7))
                        continue;
                    if (!regions.Committed(static_cast<std::uintptr_t>(value), queryRegion))
                        continue;
                    Hits& hits = found[{kind, offset}];
                    if (value >= imageBase && value < imageEnd)
                        ++hits.image;
                    else
                        ++hits.other;
                }
            };
            for (const Blob& blob : clip)
                scan(blob.kind, blob.bytes.data(), blob.bytes.size());
            std::ostringstream text;
            for (const auto& [key, hits] : found)
                text << key.first << "+0x" << std::hex << key.second << std::dec << "=" << hits.image << "i/" << hits.other << "o;";
            std::ostringstream counts;
            for (const auto& [kind, count] : blobs)
                counts << kind << "=" << count << ";";
            queries = regions.Queries();
            return "blobs=" + counts.str() + ",pointers=" + (found.empty() ? std::string("none") : text.str());
        }
    }

    std::string AuditV176(const std::vector<Blob>& clip, std::uintptr_t imageBase, std::uintptr_t imageEnd, FakeSpace& space,
        std::uint32_t& queries)
    {
        replay::ptraudit::Input input;
        for (const Blob& blob : clip)
        {
            if (blob.bytes.size() == 8)
            {
                std::uint64_t value = 0;
                std::memcpy(&value, blob.bytes.data(), 8);
                input.ScanValue(blob.kind, value);
            }
            else
                input.Scan(blob.kind, blob.bytes.data(), blob.bytes.size());
        }
        const auto query = [&space](std::uintptr_t address, std::uintptr_t& start, std::uintptr_t& end, bool& committed) {
            return space.Query(address, start, end, committed);
        };
        const replay::ptraudit::Result result = replay::ptraudit::Classify(input, imageBase, imageEnd, query);
        queries = result.queries;
        return "blobs=" + result.blobs + ",pointers=" + result.pointers;
    }

    void PutWord(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value)
    {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    void TestPointerAuditMatchesV175()
    {
        std::mt19937_64 rng(0x5EED176ull);
        std::uint64_t oldQueries = 0, newQueries = 0, oldPages = 0, newPages = 0;
        int differ = 0;
        constexpr int kCases = 300;
        for (int round = 0; round < kCases; ++round)
        {
            // The address space: free, reserved and committed runs from
            // 64 KB up, one image run, one "heap" run of up to 4 GB.
            FakeSpace space;
            std::uintptr_t at = 0x10000;
            std::uintptr_t imageBase = 0, imageEnd = 0;
            const int runCount = 4 + static_cast<int>(rng() % 40);
            for (int run = 0; run < runCount; ++run)
            {
                const bool heap = rng() % 8 == 0;
                const std::uintptr_t pages = heap ? 0x10000 + rng() % 0x100000 : 1 + rng() % 0x4000;
                FakeSpace::Run entry{at, at + pages * 0x1000, rng() % 3 != 0};
                if (!imageBase && run == runCount / 2)
                {
                    entry.committed = true;
                    imageBase = entry.start;
                    imageEnd = entry.end;
                }
                space.runs.push_back(entry);
                at = entry.end;
            }
            const std::uintptr_t spaceEnd = at;
            const auto randomWord = [&]() -> std::uint64_t {
                switch (rng() % 6)
                {
                case 0: return rng() % 0x10000;                       // small value
                case 1: return rng();                                  // anything (mostly not user space)
                case 2: return (0x10000 + rng() % (spaceEnd - 0x10000)) | (rng() % 2); // maybe unaligned
                case 3: return (spaceEnd + rng() % 0x100000000ull) & ~7ull; // past the runs: query fails
                default: return (0x10000 + rng() % (spaceEnd - 0x10000)) & ~7ull; // inside a run
                }
            };
            // Blobs in clip order: records of a few kinds and single values,
            // with repeated pointers in the same slot (melee info).
            std::vector<Blob> clip;
            static const char* const kKinds[] = {"blood_record", "gore_context", "fire_desc", "fire_rng"};
            static const char* const kValues[] = {"melee_info", "melee_info_vtable", "impact_arg4", "impact_definition"};
            const int blobCount = static_cast<int>(rng() % 60);
            const std::uint64_t shared = randomWord();
            for (int index = 0; index < blobCount; ++index)
            {
                Blob blob;
                if (rng() % 2)
                {
                    blob.kind = kKinds[rng() % 4];
                    blob.bytes.assign(8 * (1 + rng() % 40) + rng() % 8, 0);
                    for (std::size_t offset = 0; offset + 8 <= blob.bytes.size(); offset += 8)
                        PutWord(blob.bytes, offset, rng() % 4 == 0 ? shared : randomWord());
                }
                else
                {
                    blob.kind = kValues[rng() % 4];
                    blob.bytes.assign(8, 0);
                    PutWord(blob.bytes, 0, rng() % 3 == 0 ? shared : randomWord());
                }
                clip.push_back(std::move(blob));
            }
            FakeSpace oldSpace = space;
            std::uint32_t oldCount = 0;
            const std::string reference = v175::Audit(clip, imageBase, imageEnd,
                [&oldSpace](std::uintptr_t address, std::uintptr_t& start, std::uintptr_t& end, bool& committed) {
                    return oldSpace.Query(address, start, end, committed);
                },
                oldCount);
            FakeSpace newSpace = space;
            std::uint32_t newCount = 0;
            const std::string audited = AuditV176(clip, imageBase, imageEnd, newSpace, newCount);
            if (audited != reference)
            {
                if (++differ <= 3)
                    std::fprintf(stderr, "audit text differs:\n  v175 %s\n  v176 %s\n", reference.c_str(), audited.c_str());
            }
            Check(newCount <= oldCount, "the sorted scan never asks more than the blob order");
            oldQueries += oldCount;
            newQueries += newCount;
            oldPages += oldSpace.pagesWalked;
            newPages += newSpace.pagesWalked;
        }
        Check(differ == 0, "the clip pointer audit text is the reference text");
        Check(newPages <= oldPages, "the sorted scan walks no more pages");
        std::printf("Clip pointer audit: %d cases, %d differ; queries %llu -> %llu, pages walked %llu -> %llu.\n", kCases, differ,
            static_cast<unsigned long long>(oldQueries), static_cast<unsigned long long>(newQueries),
            static_cast<unsigned long long>(oldPages), static_cast<unsigned long long>(newPages));
    }

    // The fist fight case: melee info pointers into one heap run of 2 GB,
    // in clip order from the top of the run down.
    void TestPointerAuditOneHeapRun()
    {
        FakeSpace space;
        space.runs.push_back({0x10000, 0x140000000ull, false});
        space.runs.push_back({0x140000000ull, 0x146000000ull, true});         // image
        space.runs.push_back({0x146000000ull, 0x200000000ull, false});
        space.runs.push_back({0x200000000ull, 0x280000000ull, true});         // heap, 2 GB
        space.runs.push_back({0x280000000ull, 0x7FFFFFFF0000ull, false});
        std::vector<Blob> clip;
        for (int hit = 0; hit < 31; ++hit)
        {
            Blob info{"melee_info", std::vector<std::uint8_t>(8)};
            PutWord(info.bytes, 0, 0x27F000000ull - static_cast<std::uint64_t>(hit) * 0x3000000ull);
            Blob vtable{"melee_info_vtable", std::vector<std::uint8_t>(8)};
            PutWord(vtable.bytes, 0, 0x142000000ull + 0x100);
            clip.push_back(std::move(info));
            clip.push_back(std::move(vtable));
        }
        FakeSpace oldSpace = space;
        std::uint32_t oldCount = 0;
        const std::string reference = v175::Audit(clip, 0x140000000ull, 0x146000000ull,
            [&oldSpace](std::uintptr_t address, std::uintptr_t& start, std::uintptr_t& end, bool& committed) {
                return oldSpace.Query(address, start, end, committed);
            },
            oldCount);
        FakeSpace newSpace = space;
        std::uint32_t newCount = 0;
        const std::string audited = AuditV176(clip, 0x140000000ull, 0x146000000ull, newSpace, newCount);
        Check(audited == reference, "one heap run: same text");
        Check(audited == "blobs=melee_info=31;melee_info_vtable=31;,pointers=melee_info+0x0=0i/31o;melee_info_vtable+0x0=31i/0o;",
            "one heap run: the expected text");
        Check(oldCount == 32 && newCount == 2, "one heap run: one query per run, not one per lower word");
        Check(newSpace.pagesWalked * 10 < oldSpace.pagesWalked, "one heap run: a tenth of the pages walked or less");
        std::printf("Clip pointer audit, one 2 GB heap run, 31 melee hits: queries %u -> %u, pages walked %llu -> %llu.\n",
            oldCount, newCount, static_cast<unsigned long long>(oldSpace.pagesWalked),
            static_cast<unsigned long long>(newSpace.pagesWalked));
    }

    void TestDeferredTake()
    {
        using replay::deferredlog::Queue;
        const std::string text = "AAAA\nBB\nCCCCCC\n";
        Check(Queue::Take(text, 0, std::string::npos, false) == text.size(), "no budget: everything");
        Check(Queue::Take(text, 0, 8, false) == 8, "the budget ends on a line end: up to it");
        Check(Queue::Take(text, 0, 7, false) == 5, "the budget ends inside a line: the lines before it");
        Check(Queue::Take(text, 0, 3, false) == 0, "no whole line fits and something was written: nothing");
        Check(Queue::Take(text, 0, 3, true) == 5, "no whole line fits and nothing was written: the first line");
        Check(Queue::Take(text, 5, 3, false) == 3, "from a line start: a line that fits");
        Check(Queue::Take(text, 8, 0, true) == 7, "no room and nothing written: the first line");
        Check(Queue::Take(text, 8, 0, false) == 0, "no room: nothing");
        Check(Queue::Take(std::string(), 0, 0, true) == 0, "an empty text: nothing");
    }

    void TestDeferredOrderBudgetQuiet()
    {
        replay::deferredlog::Queue queue;
        const bool quietBefore = replay::log::Quiet();
        // Job 1 is the slowest: the others wait for it, in order.
        replay::log::SetQuiet(false);
        queue.Submit([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            std::string text;
            for (int line = 0; line < 50; ++line)
                text += "JOB1,line=" + std::to_string(line) + "\n";
            return text;
        });
        replay::log::SetQuiet(true); // the always-on buffer's tick
        queue.Submit([]() { return std::string("JOB2,quiet=1\n"); });
        replay::log::SetQuiet(false);
        queue.Submit([]() { return std::string(); });
        queue.Submit([]() -> std::string { throw std::runtime_error("no memory"); });
        queue.Submit([]() { return std::string("JOB5,last\n"); });
        Check(queue.Pending() == 5, "five jobs pending");

        std::string written;
        std::vector<bool> quiets;
        std::vector<std::string> lines;
        const auto write = [&](std::string_view text, bool quiet) {
            Check(!text.empty() && text.back() == '\n', "whole lines only");
            written.append(text.data(), text.size());
            std::size_t from = 0;
            while (from < text.size())
            {
                const std::size_t end = text.find('\n', from);
                lines.emplace_back(text.substr(from, end - from));
                quiets.push_back(quiet);
                from = end + 1;
            }
        };
        // Nothing before job 1 is done (it sleeps), then 64 bytes a call.
        Check(queue.Poll(write, 64) == 0 && written.empty(), "an unfinished first job holds the queue");
        int calls = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (queue.Pending() > 0 && std::chrono::steady_clock::now() < until)
        {
            const std::size_t bytes = queue.Poll(write, 64);
            Check(bytes <= 64, "at most the budget per call (lines shorter than it)");
            ++calls;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(queue.Pending() == 0, "every job written");
        Check(calls > 10, "the budget spreads the text over calls");
        std::vector<std::string> expected;
        for (int line = 0; line < 50; ++line)
            expected.push_back("JOB1,line=" + std::to_string(line));
        expected.push_back("JOB2,quiet=1");
        expected.push_back("DEFERRED_LOG_ERROR,what=exception");
        expected.push_back("JOB5,last");
        Check(lines == expected, "the jobs' lines in the order they were asked for");
        bool quietRight = quiets.size() == expected.size();
        for (std::size_t index = 0; quietRight && index < quiets.size(); ++index)
            quietRight = quiets[index] == (expected[index] == "JOB2,quiet=1");
        Check(quietRight, "each job is written with the quiet state of the tick that asked for it");

        // Drain: waits and writes everything at once.
        replay::deferredlog::Queue other;
        other.Submit([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return std::string("A\nB\n");
        });
        other.Submit([]() { return std::string("C\n"); });
        std::string drained;
        other.Drain([&](std::string_view text, bool) { drained.append(text.data(), text.size()); });
        Check(drained == "A\nB\nC\n" && other.Pending() == 0, "Drain waits for every job, in order");
        replay::log::SetQuiet(quietBefore);
        std::printf("Deferred log: %zu lines in %d polls of 64 bytes, order and quiet kept.\n", lines.size(), calls);
    }
}

int RunStopPathTests()
{
    TestPointerAuditMatchesV175();
    TestPointerAuditOneHeapRun();
    TestDeferredTake();
    TestDeferredOrderBudgetQuiet();
    if (g_stopPathFailures == 0)
        std::printf("Stop path tests passed.\n");
    return g_stopPathFailures;
}
