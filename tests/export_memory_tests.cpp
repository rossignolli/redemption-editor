// Render export memory telemetry (editor/render_export/export_memory.h): the
// line's fields in MB, the missing adapter, the peaks and the 2 s cadence
// for any frame rate.
#include "../editor/render_export/export_memory.h"

#include <cstdio>
#include <string>

namespace
{
    namespace re = render_export;
    namespace ex = replay::exporting;

    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    void TestLine()
    {
        re::MemorySample s;
        s.vram = true;
        s.vramUsed = 9000ull << 20;
        s.vramBudget = (15000ull << 20) + 123;
        s.sharedUsed = 700ull << 20;
        s.sharedBudget = 16000ull << 20;
        s.privateBytes = 12345ull << 20;
        s.commit = 30000ull << 20;
        s.commitLimit = 40000ull << 20;
        const std::string line = re::FormatMemLine(s, 1200, 36000, 3, 7, "periodic");
        Check(line == "RENDER_EXPORT_MEM,frame=1200,of=36000,vram_used_mb=9000,vram_budget_mb=15000,shared_used_mb=700,shared_budget_mb=16000,"
                      "private_mb=12345,commit_mb=30000,commit_limit_mb=40000,inflight_slots=3,encoder_queue=7,reason=periodic",
            "the line's fields, in whole MB");
        s.vram = false;
        const std::string none = re::FormatMemLine(s, 0, 0, 0, 0, "arm");
        Check(none.find("vram_used_mb=-1,vram_budget_mb=-1,shared_used_mb=-1,shared_budget_mb=-1,private_mb=12345") != std::string::npos,
            "no adapter: -1 in the video fields, the process values still there");
    }

    void TestPeaks()
    {
        re::MemoryPeaks peaks;
        re::MemorySample a, b;
        a.vramUsed = 5ull << 20;
        a.privateBytes = 9ull << 20;
        a.commit = 3ull << 20;
        b.vramUsed = 7ull << 20;
        b.sharedUsed = 2ull << 20;
        b.privateBytes = 4ull << 20;
        b.commit = 8ull << 20;
        peaks.Add(a);
        peaks.Add(b);
        Check(peaks.vramUsed == (7ull << 20) && peaks.sharedUsed == (2ull << 20) && peaks.privateBytes == (9ull << 20) &&
                peaks.commit == (8ull << 20) && peaks.samples == 2,
            "each peak is its own maximum");
        Check(re::FormatMemPeaks(peaks) == ",peak_vram_mb=7,peak_shared_mb=2,peak_private_mb=9,peak_commit_mb=8,mem_samples=2",
            "the finish line's peaks");
    }

    void TestCadence()
    {
        Check(re::MemLogEveryFrames(ex::FrameRate{60, 1}) == 120, "60 fps: every 120 frames");
        Check(re::MemLogEveryFrames(ex::FrameRate{30000, 1001}) == 59, "29.97 fps: every 59 frames");
        Check(re::MemLogEveryFrames(ex::FrameRate{24, 1}) == 48, "24 fps: every 48 frames");
        Check(re::MemLogEveryFrames(ex::FrameRate{1, 1}) == 2, "1 fps: every 2 frames");
        Check(re::MemLogEveryFrames(ex::FrameRate{0, 1}) == 120, "an invalid rate: the 60 fps cadence");
    }
}

int RunExportMemoryTests()
{
    g_failures = 0;
    TestLine();
    TestPeaks();
    TestCadence();
    std::printf("export memory tests: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures;
}
