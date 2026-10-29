#pragma once

// Render export: the GPU side in the add-on (render thread only).
//
// Per Capture tick (one game frame = one motion blur sample), from the
// reshade_overlay callback (the game and the user's ReShade effects in the
// back buffer, the editor's UI not drawn yet):
//   1. copy the back buffer into a scratch texture (GPU to GPU, as
//      seek_freeze.h does);
//   2. with motion blur, CSAccumulate adds it to a float4 sum buffer;
//   3. on the frame's last sample, CSResolve writes the output frame in the
//      encoder's layout (NV12 / P010 / RGBA8 / X2BGR10, letterbox and grain
//      applied) into a byte buffer, which is copied into a free slot of a
//      ring of host visible staging buffers.
// Nothing waits for the GPU on the frame just submitted: a slot is mapped
// on a later present, when its end timestamp is available (the timestamp
// is written after the copy; a value newer than the slot's previous one
// proves the copy of this lap finished). The immediate command list is
// never flushed from here (a flush in the overlay would submit before the
// present's wait semaphores), except once in Begin: the wait_idle that makes
// sure the query heaps' creation reset ran before any query is read. Every
// query read goes through QueryResults, which uses the get_query_heap_results
// signature of the ReShade loaded (reshade_query_abi.h). When every slot is in flight the oldest is
// waited for by polling its timestamp: it was submitted on an earlier
// present, so it completes without a flush (backpressure, never a drop).
//
// The compute shaders are SPIR-V (shaders/export_convert_spirv.h, Vulkan,
// the game's API). Under D3D12 Begin refuses (no DXIL yet): the export
// fails with UnsupportedApi in the log.

#include "convert_ref.h"
#include "export_channel.h"
#include "reshade_query_abi.h"

#include <reshade.hpp>

#include <Windows.h>

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace render_export
{
    struct GpuConfig
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        reshade::api::format backFormat = reshade::api::format::unknown;
        OutputLayout layout = OutputLayout::Nv12;
        bool accumulate = false; // motion blur (samples > 1)
        std::uint32_t transfer = 0; // 0 sRGB, 1 PQ, 2 linear
        bool bt2020 = false;
        bool dither = true;
        std::uint32_t barTop = 0;
        std::uint32_t barBottom = 0;
        float grain = 0.0f; // code values / 255 as in Settings::grain, scaled to 0..1 here
        std::uint32_t slots = 6;
    };

    // Running averages (us) of the stages, published to the channel.
    struct GpuTimings
    {
        double copyUs = 0.0;
        double accumulateUs = 0.0;
        double resolveUs = 0.0;
        double readbackUs = 0.0;
        double mapUs = 0.0;
        double ringWaitUs = 0.0;
    };

    // memcpy split over a few persistent threads (the caller takes a part
    // too and returns when every part is done). The readback copy of a 4K
    // frame (12 to 25 MB out of host cached memory) takes 1.2 to 2.6 ms on
    // the render thread with one thread.
    class ParallelCopy
    {
    public:
        // Threads only between Start and Stop (Begin and End on the render
        // thread): never created or joined under the loader lock (the core
        // DLL is a static object swapped in and out by the loader).
        ParallelCopy() = default;
        ~ParallelCopy();
        void Start(std::uint32_t helpers);
        void Stop();
        ParallelCopy(const ParallelCopy&) = delete;
        ParallelCopy& operator=(const ParallelCopy&) = delete;
        void Copy(void* destination, const void* source, std::size_t bytes);

    private:
        void Loop(std::uint32_t index, std::uint64_t seen);
        std::vector<std::thread> m_threads;
        std::mutex m_mutex;
        std::condition_variable m_start;
        std::condition_variable m_done;
        std::uint8_t* m_destination = nullptr;
        const std::uint8_t* m_source = nullptr;
        std::size_t m_bytes = 0;
        std::uint64_t m_generation = 0;
        std::uint32_t m_pending = 0;
        bool m_quit = false;
    };

    // The ABI of get_query_heap_results in the ReShade module loaded in this
    // process (reshade_query_abi.h has why). `versionText`: its ReShadeVersion,
    // empty when the module or the export is missing. Looked up once.
    QueryAbi DetectReShadeQueryAbi(std::string& versionText);

    // The longest Drain(wait = true) blocks on one slot before it gives up
    // (a query that never becomes available: wrong ABI, device lost, a driver
    // that returns an error). The export then fails instead of hanging the
    // render thread.
    constexpr unsigned long long kDrainWaitLimitMs = 2000;

    class GpuExporter
    {
    public:
        bool Begin(reshade::api::effect_runtime* runtime, const GpuConfig& config, std::string& error);
        // One Capture tick. `back` is the current back buffer.
        bool Sample(reshade::api::effect_runtime* runtime, float weight, bool first, bool last, std::int64_t frame, std::uint32_t seed,
            std::string& error);
        // Hands every finished frame, oldest first, to `sink(frame, bytes)`
        // (returns false to stop). `wait`: block for the oldest slot if it is
        // not done yet (only slots recorded on an earlier present: those were
        // submitted, so they complete without a flush), then go on without
        // blocking.
        template <typename Sink>
        bool Drain(reshade::api::effect_runtime* runtime, bool wait, Sink&& sink);
        // Once per present (the slots recorded before it are submitted).
        void OnPresent() { ++m_present; }
        bool Full() const { return InFlight() >= m_slots.size(); }
        std::uint32_t InFlight() const;
        void End(reshade::api::device* device);
        bool Ready() const { return m_ready; }
        const GpuTimings& Timings() const { return m_timings; }
        // The last Drain returned false because the wait passed kDrainWaitLimitMs.
        bool DrainTimedOut() const { return m_drainTimedOut; }
        QueryAbi Abi() const { return m_queryAbi; }
        std::uint64_t FrameBytes() const { return m_frameBytes; }

        // Latency tag reader (preroll and flush pictures): copies the tag
        // cells' centre pixels; Decode on a later present.
        void ReadTag(reshade::api::effect_runtime* runtime, std::uint64_t presentIndex);
        // Every tag read finished so far: (present index, tag) pairs.
        template <typename Sink>
        void DrainTags(reshade::api::device* device, Sink&& sink);

    private:
        struct Slot
        {
            reshade::api::resource buffer{0};
            void* mapped = nullptr; // persistent map (Begin to End); null: mapped per frame
            std::int64_t frame = -1;
            bool inFlight = false;
            std::uint64_t order = 0;
            std::uint64_t lastEndTimestamp = 0;
            std::uint64_t recordedPresent = 0;
        };
        struct TagSlot
        {
            reshade::api::resource buffer{0};
            bool inFlight = false;
            std::uint64_t presentIndex = 0;
            std::uint64_t lastTimestamp = 0;
        };

        // Every read of a timestamp query goes through here: the signature of
        // the loaded ReShade (Begin refuses an Unknown one).
        bool QueryResults(reshade::api::device* device, reshade::api::query_heap heap, std::uint32_t first, std::uint32_t count,
            void* results, std::uint32_t stride);
        bool SlotDone(reshade::api::device* device, Slot& slot, std::uint32_t index);
        bool TakeSlot(reshade::api::device* device, std::uint32_t index, std::vector<std::uint8_t>& out);
        void Accumulate(double& average, double sample) { average = average == 0.0 ? sample : average * 0.9 + sample * 0.1; }
        static double ElapsedUs(std::uint64_t a, std::uint64_t b, std::uint64_t frequency);

        GpuConfig m_config;
        bool m_ready = false;
        reshade::api::device* m_device = nullptr;
        reshade::api::resource m_scratch{0};
        reshade::api::resource_view m_scratchSrv{0};
        reshade::api::resource m_sum{0};
        reshade::api::resource m_output{0};
        reshade::api::resource_view m_sumUav{0};
        reshade::api::resource_view m_outputUav{0};
        std::uint32_t m_outputRows = 0;
        std::uint64_t m_readbackBytes = 0; // whole output image (>= m_frameBytes)
        reshade::api::pipeline_layout m_layout{0};
        reshade::api::pipeline m_accumulate{0};
        reshade::api::pipeline m_resolve{0};
        reshade::api::query_heap m_queries{0};
        std::uint64_t m_timestampFrequency = 0;
        std::uint64_t m_frameBytes = 0;
        std::uint64_t m_sequence = 0;
        std::uint64_t m_present = 0;
        std::vector<Slot> m_slots;
        std::vector<TagSlot> m_tags;
        std::vector<std::uint8_t> m_takeBuffer;
        ParallelCopy m_copy;
        GpuTimings m_timings;
        std::uint32_t m_tagNext = 0;
        reshade::api::query_heap m_tagQueries{0};
        std::uint32_t m_tagBytesPerPixel = 4;
        bool m_tag10Bit = false;
        QueryAbi m_queryAbi = QueryAbi::Unknown;
        bool m_drainTimedOut = false;

        static constexpr std::uint32_t kQueriesPerSlot = 5; // copy start, copy end, accumulate end, resolve end, readback end
        static constexpr std::uint32_t kTagSlots = 8;
        static constexpr std::uint32_t kTagStride = 512; // bytes per cell in the tag buffer (D3D12 placement alignment)
    };

    // ------------------------------------------------------------------
    // Templates
    // ------------------------------------------------------------------

    template <typename Sink>
    bool GpuExporter::Drain(reshade::api::effect_runtime* runtime, bool wait, Sink&& sink)
    {
        if (!m_ready)
            return true;
        reshade::api::device* device = runtime->get_device();
        m_drainTimedOut = false;
        const ULONGLONG waitStart = GetTickCount64();
        for (;;)
        {
            // The oldest slot in flight (frames leave in order).
            std::uint32_t oldest = ~0u;
            for (std::uint32_t i = 0; i < m_slots.size(); ++i)
                if (m_slots[i].inFlight && (oldest == ~0u || m_slots[i].order < m_slots[oldest].order))
                    oldest = i;
            if (oldest == ~0u)
                return true;
            if (!SlotDone(device, m_slots[oldest], oldest))
            {
                if (!wait || m_slots[oldest].recordedPresent >= m_present)
                    return true;
                if (GetTickCount64() - waitStart > kDrainWaitLimitMs)
                {
                    m_drainTimedOut = true;
                    return false;
                }
                Sleep(0);
                continue;
            }
            wait = false;
            const std::int64_t frame = m_slots[oldest].frame;
            if (!TakeSlot(device, oldest, m_takeBuffer))
                return false;
            if (!sink(frame, m_takeBuffer))
                return false;
        }
    }

    template <typename Sink>
    void GpuExporter::DrainTags(reshade::api::device* device, Sink&& sink)
    {
        for (TagSlot& tag : m_tags)
        {
            if (!tag.inFlight)
                continue;
            const std::uint32_t index = static_cast<std::uint32_t>(&tag - m_tags.data());
            std::uint64_t timestamp = 0;
            if (!QueryResults(device, m_tagQueries, index, 1, &timestamp, sizeof(timestamp)) || timestamp == 0 ||
                timestamp <= tag.lastTimestamp)
                continue;
            tag.lastTimestamp = timestamp;
            tag.inFlight = false;
            void* data = nullptr;
            if (!device->map_buffer_region(tag.buffer, 0, UINT64_MAX, reshade::api::map_access::read_only, &data) || !data)
                continue;
            bool white[replay::exportchannel::kTagCells] = {};
            for (std::uint32_t cell = 0; cell < replay::exportchannel::kTagCells; ++cell)
            {
                const auto* p = static_cast<const std::uint8_t*>(data) + cell * kTagStride;
                std::uint32_t r, g, b;
                if (m_tag10Bit)
                {
                    std::uint32_t word;
                    std::memcpy(&word, p, 4);
                    r = (word & 0x3FF) >> 2;
                    g = ((word >> 10) & 0x3FF) >> 2;
                    b = ((word >> 20) & 0x3FF) >> 2;
                }
                else
                {
                    r = p[0];
                    g = p[1];
                    b = p[2];
                }
                white[cell] = (r + g + b) > 3u * 128u;
            }
            device->unmap_buffer_region(tag.buffer);
            std::uint32_t value = 0;
            if (replay::exportchannel::DecodeTag(white, value))
                sink(tag.presentIndex, value);
        }
    }
}
