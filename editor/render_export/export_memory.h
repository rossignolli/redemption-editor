#pragma once

// Render export: memory telemetry. A long export that dies near its end
// (GitHub issue #1: 10 minute videos, 16 GB of VRAM and RAM, a crash around
// 80 %) leaves no trace of what grew. Every 2 s of video, at the arm, at the
// finish and on a failure the export log gets one RENDER_EXPORT_MEM line with
// the game adapter's video memory (DXGI segment groups: dedicated and
// shared), the process's private bytes and the system's commit, so a curve
// shows growth against a peak. The finish line carries the peaks.
//
// The values and the lines are pure (unit tested in ReplayCoreTests,
// tests/export_memory_tests.cpp); MemoryProbe (export_memory.cpp) reads
// them from DXGI and the process.

#include "export_schedule.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace render_export
{
    // One reading, bytes.
    struct MemorySample
    {
        bool vram = false;              // the adapter was found (the four video fields)
        std::uint64_t vramUsed = 0;     // DXGI local segment group: this process's usage
        std::uint64_t vramBudget = 0;   // what the OS lets this process use there
        std::uint64_t sharedUsed = 0;   // non local group (system memory the GPU maps)
        std::uint64_t sharedBudget = 0;
        std::uint64_t privateBytes = 0; // the process's commit (PROCESS_MEMORY_COUNTERS_EX PrivateUsage)
        std::uint64_t commit = 0;       // the system's commit charge
        std::uint64_t commitLimit = 0;
    };

    // The highest of each value over an export.
    struct MemoryPeaks
    {
        std::uint64_t vramUsed = 0;
        std::uint64_t sharedUsed = 0;
        std::uint64_t privateBytes = 0;
        std::uint64_t commit = 0;
        std::uint32_t samples = 0;

        void Add(const MemorySample& sample) noexcept
        {
            vramUsed = sample.vramUsed > vramUsed ? sample.vramUsed : vramUsed;
            sharedUsed = sample.sharedUsed > sharedUsed ? sample.sharedUsed : sharedUsed;
            privateBytes = sample.privateBytes > privateBytes ? sample.privateBytes : privateBytes;
            commit = sample.commit > commit ? sample.commit : commit;
            ++samples;
        }
    };

    constexpr std::uint64_t MemMb(std::uint64_t bytes) noexcept { return bytes >> 20; }

    // Output frames between two periodic lines: 2 s of video at the export's
    // rate (at least 1), so a 10 minute export logs about 300 lines whatever
    // the PC's speed.
    inline std::uint64_t MemLogEveryFrames(replay::exporting::FrameRate rate) noexcept
    {
        if (!replay::exporting::ValidRate(rate))
            return 120;
        const std::uint64_t frames = 2ull * rate.num / rate.den;
        return frames ? frames : 1;
    }

    // RENDER_EXPORT_MEM,frame=..,of=..,vram_used_mb=..,... ,reason=periodic
    // (-1 in the video fields when no adapter was found).
    inline std::string FormatMemLine(const MemorySample& s, std::uint64_t frame, std::uint64_t of, std::uint32_t inflightSlots,
        std::uint32_t encoderQueue, const char* reason)
    {
        const auto mb = [&s](std::uint64_t bytes) { return s.vram ? static_cast<long long>(MemMb(bytes)) : -1ll; };
        char line[400];
        std::snprintf(line, sizeof(line),
            "RENDER_EXPORT_MEM,frame=%llu,of=%llu,vram_used_mb=%lld,vram_budget_mb=%lld,shared_used_mb=%lld,shared_budget_mb=%lld,private_mb=%llu,"
            "commit_mb=%llu,commit_limit_mb=%llu,inflight_slots=%u,encoder_queue=%u,reason=%s",
            static_cast<unsigned long long>(frame), static_cast<unsigned long long>(of), mb(s.vramUsed), mb(s.vramBudget), mb(s.sharedUsed),
            mb(s.sharedBudget), static_cast<unsigned long long>(MemMb(s.privateBytes)), static_cast<unsigned long long>(MemMb(s.commit)),
            static_cast<unsigned long long>(MemMb(s.commitLimit)), inflightSlots, encoderQueue, reason ? reason : "?");
        return line;
    }

    // ",peak_vram_mb=..,peak_shared_mb=..,peak_private_mb=..,peak_commit_mb=..,mem_samples=.."
    // for the RENDER_EXPORT done and failed lines.
    inline std::string FormatMemPeaks(const MemoryPeaks& p)
    {
        char text[200];
        std::snprintf(text, sizeof(text), ",peak_vram_mb=%llu,peak_shared_mb=%llu,peak_private_mb=%llu,peak_commit_mb=%llu,mem_samples=%u",
            static_cast<unsigned long long>(MemMb(p.vramUsed)), static_cast<unsigned long long>(MemMb(p.sharedUsed)),
            static_cast<unsigned long long>(MemMb(p.privateBytes)), static_cast<unsigned long long>(MemMb(p.commit)), p.samples);
        return text;
    }

    // Reads the values (export_memory.cpp). Open finds the game's adapter:
    // by its LUID on D3D12; on Vulkan (no adapter handle in ReShade's API) the
    // adapter where this process holds the most dedicated memory, else the
    // one with the most dedicated memory. Cheap enough for the render thread
    // (QueryVideoMemoryInfo reads a kernel counter); thread safe.
    class MemoryProbe
    {
    public:
        MemoryProbe() = default;
        ~MemoryProbe() { Close(); }
        MemoryProbe(const MemoryProbe&) = delete;
        MemoryProbe& operator=(const MemoryProbe&) = delete;

        // `nativeDevice`: reshade::api::device::get_native (an ID3D12Device
        // when `d3d12`). A RENDER_EXPORT line naming the adapter and how it
        // was matched.
        std::string Open(std::uint64_t nativeDevice, bool d3d12);
        void Close();
        MemorySample Sample() const;

    private:
        void* m_adapter = nullptr; // IDXGIAdapter3
    };
}
