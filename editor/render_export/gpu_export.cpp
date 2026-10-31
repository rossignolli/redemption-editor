// Render export: the GPU side in the add-on (gpu_export.h has the design).

#include "gpu_export.h"

#include "shaders/export_convert_dxil.h"
#include "shaders/export_convert_spirv.h"
#include "thread_names.h"

#include <algorithm>

namespace render_export
{
    using namespace reshade::api;

    namespace
    {
        constexpr std::uint32_t kConstantWords = sizeof(ExportConstants) / 4;

        // D3D12 copies between a texture and a buffer in rows of a multiple
        // of D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256 bytes; ReShade's D3D12
        // copy_texture_to_buffer rounds the row up to it). The output image
        // row is exactly that, so its bytes stay tight in the staging slot
        // under both APIs.
        constexpr std::uint32_t kD3D12RowPitchAlignment = 256;
        static_assert((kOutputRowWords * 4) % kD3D12RowPitchAlignment == 0, "output row must be a multiple of the D3D12 row pitch");
        // Tag cells are copied at kTagStride offsets: D3D12 placed footprints
        // start on D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512).
        constexpr std::uint32_t kD3D12PlacementAlignment = 512;

        const char* ApiName(device_api api)
        {
            switch (api)
            {
            case device_api::vulkan: return "vulkan";
            case device_api::d3d12: return "d3d12";
            case device_api::d3d11: return "d3d11";
            case device_api::d3d10: return "d3d10";
            case device_api::d3d9: return "d3d9";
            case device_api::opengl: return "opengl";
            }
            return "unknown";
        }

        const char* FormatName(format f)
        {
            switch (f)
            {
            case format::r8g8b8a8_unorm: return "r8g8b8a8_unorm";
            case format::r8g8b8a8_unorm_srgb: return "r8g8b8a8_unorm_srgb";
            case format::r8g8b8a8_typeless: return "r8g8b8a8_typeless";
            case format::b8g8r8a8_unorm: return "b8g8r8a8_unorm";
            case format::b8g8r8a8_unorm_srgb: return "b8g8r8a8_unorm_srgb";
            case format::b8g8r8a8_typeless: return "b8g8r8a8_typeless";
            case format::b8g8r8x8_unorm: return "b8g8r8x8_unorm";
            case format::r10g10b10a2_unorm: return "r10g10b10a2_unorm";
            case format::r10g10b10a2_typeless: return "r10g10b10a2_typeless";
            case format::b10g10r10a2_unorm: return "b10g10r10a2_unorm";
            case format::r16g16b16a16_float: return "r16g16b16a16_float";
            default: return "other";
            }
        }
    }

    // CallQueryResultsApi20 passes these as plain integers.
    static_assert(sizeof(query_heap) == sizeof(std::uint64_t), "query_heap is one 64 bit handle");
    static_assert(sizeof(query_type) == sizeof(std::uint32_t), "query_type is a 32 bit enum");

    QueryAbi DetectReShadeQueryAbi(std::string& versionText)
    {
        // The module that exports ReShadeRegisterAddon (the one this add-on
        // registered with); ReShadeVersion is an exported `const char*`, so
        // GetProcAddress gives the address of the pointer.
        static const std::string s_version = [] {
            const HMODULE module = reshade::internal::get_reshade_module_handle();
            if (!module)
                return std::string();
            // Under the editor's own host (host/host_log.cpp) the same text is
            // RedemptionEditorHostVersion: a ReShadeVersion export would stop ReShade.
            const auto* exported = reinterpret_cast<const char* const*>(GetProcAddress(module, "ReShadeVersion"));
            if (!exported)
                exported = reinterpret_cast<const char* const*>(GetProcAddress(module, "RedemptionEditorHostVersion"));
            if (!exported || !*exported)
                return std::string();
            return std::string(*exported).substr(0, 64);
        }();
        versionText = s_version;
        return s_version.empty() ? QueryAbi::Unknown : QueryAbiForVersion(s_version.c_str());
    }

    bool GpuExporter::QueryResults(device* dev, query_heap heap, std::uint32_t first, std::uint32_t count, void* results, std::uint32_t stride)
    {
        switch (m_queryAbi)
        {
        case QueryAbi::Api18:
            return dev->get_query_heap_results(heap, first, count, results, stride);
        case QueryAbi::Api20:
            // Every heap of the export holds timestamps (Vulkan ignores the type).
            return CallQueryResultsApi20(dev, heap.handle, static_cast<std::uint32_t>(query_type::timestamp), first, count, results, stride);
        default:
            return false; // Begin refuses before any query exists
        }
    }

    void ParallelCopy::Start(std::uint32_t helpers)
    {
        Stop();
        m_quit = false;
        const std::uint64_t generation = m_generation; // jobs before this start are not theirs
        for (std::uint32_t i = 0; i < helpers; ++i)
            m_threads.emplace_back([this, i, generation] {
                char name[24];
                std::snprintf(name, sizeof(name), "export_readback%u", i);
                replay::threads::Scope threadName(name);
                Loop(i, generation);
            });
    }

    void ParallelCopy::Stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_start.notify_all();
        for (std::thread& thread : m_threads)
            thread.join();
        m_threads.clear();
    }

    ParallelCopy::~ParallelCopy()
    {
        // Only reached with threads when the DLL unloads in the middle of an
        // export: no join under the loader lock; they leave on m_quit.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_start.notify_all();
        for (std::thread& thread : m_threads)
            thread.detach();
    }

    void ParallelCopy::Loop(std::uint32_t index, std::uint64_t seen)
    {
        for (;;)
        {
            std::uint8_t* destination;
            const std::uint8_t* source;
            std::size_t bytes;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_start.wait(lock, [&] { return m_quit || m_generation != seen; });
                if (m_quit)
                    return;
                seen = m_generation;
                destination = m_destination;
                source = m_source;
                bytes = m_bytes;
            }
            // Parts of 64 byte multiples; part 0 is the caller's.
            const std::size_t parts = m_threads.size() + 1;
            const std::size_t part = (bytes / parts) & ~static_cast<std::size_t>(63);
            const std::size_t begin = part * (index + 1);
            const std::size_t end = index + 2 == parts ? bytes : begin + part;
            if (end > begin)
                std::memcpy(destination + begin, source + begin, end - begin);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (--m_pending == 0)
                    m_done.notify_one();
            }
        }
    }

    void ParallelCopy::Copy(void* destination, const void* source, std::size_t bytes)
    {
        if (m_threads.empty() || bytes < (1u << 20))
        {
            std::memcpy(destination, source, bytes);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_destination = static_cast<std::uint8_t*>(destination);
            m_source = static_cast<const std::uint8_t*>(source);
            m_bytes = bytes;
            m_pending = static_cast<std::uint32_t>(m_threads.size());
            ++m_generation;
        }
        m_start.notify_all();
        const std::size_t part = (bytes / (m_threads.size() + 1)) & ~static_cast<std::size_t>(63);
        std::memcpy(destination, source, part);
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done.wait(lock, [this] { return m_pending == 0; });
    }

    double GpuExporter::ElapsedUs(std::uint64_t a, std::uint64_t b, std::uint64_t frequency)
    {
        if (!frequency || b < a)
            return 0.0;
        return static_cast<double>(b - a) * 1e6 / static_cast<double>(frequency);
    }

    bool GpuExporter::Begin(effect_runtime* runtime, const GpuConfig& config, std::string& error)
    {
        m_config = config;
        m_ready = false;
        m_timings = {};
        device* dev = runtime->get_device();
        m_device = dev;
        m_api = dev->get_api();
        // The compute shaders exist as SPIR-V (Vulkan) and DXIL (D3D12), from
        // the same HLSL (shaders/export_convert.hlsl has the bindings).
        if (m_api != device_api::vulkan && m_api != device_api::d3d12)
        {
            error = std::string("unsupported_api (") + ApiName(m_api) + "; the export runs in Vulkan or DirectX 12)";
            return false;
        }
        // D3D12: the timestamps and the compute work go on the immediate
        // command list of the queue that presents. A copy queue has neither
        // compute nor direct-queue timestamps (it would need a copy queue
        // timestamp heap): refused. Only checked under D3D12 (Vulkan as
        // before this check). A DXGI swapchain is made on a direct
        // queue, so this is a guard, not an expected case.
        {
            command_queue* queue = runtime->get_command_queue();
            m_queueType = queue ? static_cast<std::uint32_t>(queue->get_type()) : 0u;
            if (!queue || (m_api == device_api::d3d12 && (queue->get_type() & command_queue_type::compute) == 0))
            {
                error = "unsupported_api (the presenting queue has no compute; queue_type=" + std::to_string(m_queueType) + ")";
                return false;
            }
        }
        // D3D12 readback heap buffers are created in COPY_DEST and can never
        // leave it (CreateCommittedResource refuses any other state on a
        // READBACK heap): no barriers on them, the timestamp after the copy
        // (a fence per query in ReShade's D3D12 query heaps) is what proves
        // the bytes landed. Vulkan keeps its host-read barrier.
        m_readbackFixedState = m_api == device_api::d3d12;
        {
            std::string version;
            m_queryAbi = DetectReShadeQueryAbi(version);
            if (m_queryAbi == QueryAbi::Unknown)
            {
                error = "unsupported_reshade (ReShade " + (version.empty() ? std::string("version unknown") : version) +
                    " is not supported by the video export; use ReShade 6.6.2 to 6.8.x)";
                return false;
            }
        }
        if (!SizeSupported(config.width, config.height))
        {
            error = "unsupported_size (width must be a multiple of 4 and height of 2)";
            return false;
        }
        m_frameBytes = OutputBytes(config.layout, config.width, config.height);

        // Scratch copy of the back buffer: typeless texture, UNORM view (the
        // shaders read encoded values and decode them themselves), the way
        // seek_freeze.h makes its frozen picture.
        const format typeless = format_to_typeless(config.backFormat);
        const format view = format_to_default_typed(typeless, 0);
        if (!dev->create_resource(resource_desc(config.width, config.height, 1, 1, typeless, 1, memory_heap::gpu_only,
                                      resource_usage::copy_dest | resource_usage::shader_resource),
                nullptr, resource_usage::shader_resource, &m_scratch) ||
            !dev->create_resource_view(m_scratch, resource_usage::shader_resource, resource_view_desc(view), &m_scratchSrv))
        {
            error = "scratch_texture";
            End(dev);
            return false;
        }
        // The sum and the output are storage images (export_convert.hlsl says
        // why not storage buffers): RGBA32F per pixel, and the output bytes as
        // R32_UINT texels kOutputRowWords wide.
        const std::uint32_t sumWidth = config.accumulate ? config.width : 8;
        const std::uint32_t sumHeight = config.accumulate ? config.height : 8;
        m_outputRows = OutputImageRows(config.layout, config.width, config.height);
        m_readbackBytes = OutputImageBytes(config.layout, config.width, config.height);
        if (!dev->create_resource(resource_desc(sumWidth, sumHeight, 1, 1, format::r32g32b32a32_float, 1, memory_heap::gpu_only,
                                      resource_usage::unordered_access),
                nullptr, resource_usage::unordered_access, &m_sum) ||
            !dev->create_resource_view(m_sum, resource_usage::unordered_access, resource_view_desc(format::r32g32b32a32_float), &m_sumUav) ||
            !dev->create_resource(resource_desc(kOutputRowWords, m_outputRows, 1, 1, format::r32_uint, 1, memory_heap::gpu_only,
                                      resource_usage::unordered_access | resource_usage::copy_source),
                nullptr, resource_usage::unordered_access, &m_output) ||
            !dev->create_resource_view(m_output, resource_usage::unordered_access, resource_view_desc(format::r32_uint), &m_outputUav))
        {
            error = "gpu_images";
            End(dev);
            return false;
        }

        // Vulkan layout: set 0 = {0 picture, 1 sum, 2 output} as push
        // descriptors, then 12 words of push constants.
        // D3D12 layout: table {t0 picture}, table {u0 sum, u1 output}, 12 root
        // constants at b0. ReShade's D3D12 push_descriptors allocates a fresh
        // descriptor table on every call and fills only the bindings it was
        // given, so the three Vulkan-style pushes into one table would leave
        // the first two descriptors stale; one push per table instead (an SRV
        // and a UAV cannot share one push: one descriptor type per update).
        descriptor_range ranges[3];
        ranges[0].binding = 0;
        ranges[0].dx_register_index = 0;
        ranges[0].count = 1;
        ranges[0].visibility = shader_stage::compute;
        ranges[0].type = descriptor_type::texture_shader_resource_view;
        ranges[1].binding = 1;
        ranges[1].dx_register_index = 0;
        ranges[1].count = 1;
        ranges[1].visibility = shader_stage::compute;
        ranges[1].type = descriptor_type::texture_unordered_access_view;
        ranges[2].binding = 2;
        ranges[2].dx_register_index = 1;
        ranges[2].count = 1;
        ranges[2].visibility = shader_stage::compute;
        ranges[2].type = descriptor_type::texture_unordered_access_view;
        bool layoutMade = false;
        if (m_api == device_api::vulkan)
        {
            pipeline_layout_param params[2];
            params[0].type = pipeline_layout_param_type::push_descriptors_with_ranges;
            params[0].descriptor_table.count = 3;
            params[0].descriptor_table.ranges = ranges;
            params[1] = pipeline_layout_param(constant_range{0, 0, 0, kConstantWords, shader_stage::compute});
            m_constantsParam = 1;
            layoutMade = dev->create_pipeline_layout(2, params, &m_layout);
        }
        else
        {
            descriptor_range srv = ranges[0]; // t0, table offset 0
            descriptor_range uavs = ranges[1]; // u0..u1, table offsets 0..1
            uavs.binding = 0;
            uavs.dx_register_index = 0;
            uavs.count = 2;
            pipeline_layout_param params[3];
            params[0].type = pipeline_layout_param_type::push_descriptors_with_ranges;
            params[0].descriptor_table.count = 1;
            params[0].descriptor_table.ranges = &srv;
            params[1].type = pipeline_layout_param_type::push_descriptors_with_ranges;
            params[1].descriptor_table.count = 1;
            params[1].descriptor_table.ranges = &uavs;
            params[2] = pipeline_layout_param(constant_range{0, 0, 0, kConstantWords, shader_stage::compute});
            m_constantsParam = 2;
            layoutMade = dev->create_pipeline_layout(3, params, &m_layout);
        }
        if (!layoutMade)
        {
            error = "pipeline_layout";
            End(dev);
            return false;
        }
        auto makePipeline = [&](const std::uint32_t* code, std::size_t bytes, const char* entry, pipeline& out) {
            shader_desc shader;
            shader.code = code;
            shader.code_size = bytes;
            shader.entry_point = entry;
            pipeline_subobject subobject{pipeline_subobject_type::compute_shader, 1, &shader};
            return dev->create_pipeline(m_layout, 1, &subobject, &out);
        };
        const bool vk = m_api == device_api::vulkan;
        if (!(vk ? makePipeline(spirv::kAccumulate, sizeof(spirv::kAccumulate), "CSAccumulate", m_accumulate)
                 : makePipeline(dxil::kAccumulate, sizeof(dxil::kAccumulate), "CSAccumulate", m_accumulate)) ||
            !(vk ? makePipeline(spirv::kResolve, sizeof(spirv::kResolve), "CSResolve", m_resolve)
                 : makePipeline(dxil::kResolve, sizeof(dxil::kResolve), "CSResolve", m_resolve)))
        {
            error = "compute_pipelines";
            End(dev);
            return false;
        }

        // Staging ring and its timestamps.
        m_slots.assign(std::max<std::uint32_t>(2, config.slots), Slot{});
        for (Slot& slot : m_slots)
        {
            if (!dev->create_resource(resource_desc(m_readbackBytes, memory_heap::gpu_to_cpu, resource_usage::copy_dest), nullptr,
                    ReadbackInitialState(), &slot.buffer))
            {
                error = "staging_ring";
                End(dev);
                return false;
            }
            // Mapped once for the whole export (a map and unmap per frame
            // showed up in cpu_map_us); a failed map falls back to mapping
            // per frame.

            void* data = nullptr;
            if (dev->map_buffer_region(slot.buffer, 0, UINT64_MAX, map_access::read_only, &data) && data)
                slot.mapped = data;
        }
        if (!dev->create_query_heap(query_type::timestamp, static_cast<std::uint32_t>(m_slots.size()) * kQueriesPerSlot, &m_queries))
        {
            error = "timestamp_queries";
            End(dev);
            return false;
        }
        m_timestampFrequency = runtime->get_command_queue()->get_timestamp_frequency();
        m_copy.Start(3);

        // Tag reader: kTagCells pixels per read.
        {
            m_tag10Bit = format_to_typeless(config.backFormat) == format::r10g10b10a2_typeless;
            m_tagBytesPerPixel = 4;
            m_tags.assign(kTagSlots, TagSlot{});
            for (TagSlot& tag : m_tags)
                dev->create_resource(resource_desc(kTagStride * replay::exportchannel::kTagCells, memory_heap::gpu_to_cpu, resource_usage::copy_dest),
                    nullptr, ReadbackInitialState(), &tag.buffer);
            dev->create_query_heap(query_type::timestamp, kTagSlots, &m_tagQueries);
        }
        // (D3D12: no reset to wait for; the wait is harmless and once.)
        // create_query_heap recorded vkCmdResetQueryPool and submitted it
        // (ReShade flushes the immediate list there) without waiting. Until the
        // reset runs on the GPU the queries are uninitialized, and reading them
        // is undefined in Vulkan (all queries read must not be uninitialized).
        // The immediate list is empty again, so this is a plain queue wait, once
        // per export, before the first ReadTag/SlotDone can read a query.
        runtime->get_command_queue()->wait_idle();
        m_sequence = 0;
        m_ready = true;
        return true;
    }

    std::string GpuExporter::Describe() const
    {
        // Every per-API difference the export takes, for the export log.
        const bool d3d12 = m_api == device_api::d3d12;
        return std::string("api=") + ApiName(m_api) + ",bytecode=" + (d3d12 ? "dxil_cs_6_0" : "spirv") +
            ",back_format=" + FormatName(m_config.backFormat) + "(" + std::to_string(static_cast<std::uint32_t>(m_config.backFormat)) + ")" +
            ",scratch_format=" + FormatName(format_to_typeless(m_config.backFormat)) +
            ",output_row_bytes=" + std::to_string(kOutputRowWords * 4) + (d3d12 ? ",row_pitch_align=" + std::to_string(kD3D12RowPitchAlignment) : std::string()) +
            ",tag_stride=" + std::to_string(kTagStride) + ",readback_state=" + (m_readbackFixedState ? "copy_dest_fixed" : "barriers") +
            ",layout=" + (d3d12 ? "srv_table+uav_table+root_constants" : "push_set0+push_constants") + ",queue_type=" + std::to_string(m_queueType) +
            ",timestamp_hz=" + std::to_string(m_timestampFrequency);
    }

    bool GpuExporter::SlotDone(device* dev, Slot& slot, std::uint32_t index)
    {
        std::uint64_t end = 0;
        if (!QueryResults(dev, m_queries, index * kQueriesPerSlot + kQueriesPerSlot - 1, 1, &end, sizeof(end)))
            return false;
        // A value from the previous lap of this slot is still "available"
        // until this lap's reset executes: only a newer timestamp counts.
        return end != 0 && end > slot.lastEndTimestamp;
    }

    bool GpuExporter::TakeSlot(device* dev, std::uint32_t index, std::vector<std::uint8_t>& out)
    {
        Slot& slot = m_slots[index];
        std::uint64_t stamps[kQueriesPerSlot] = {};
        if (QueryResults(dev, m_queries, index * kQueriesPerSlot, kQueriesPerSlot, stamps, sizeof(std::uint64_t)))
        {
            if (stamps[1] > stamps[0])
                Accumulate(m_timings.copyUs, ElapsedUs(stamps[0], stamps[1], m_timestampFrequency));
            if (m_config.accumulate && stamps[2] >= stamps[1])
                Accumulate(m_timings.accumulateUs, ElapsedUs(stamps[1], stamps[2], m_timestampFrequency));
            if (stamps[3] >= stamps[2])
                Accumulate(m_timings.resolveUs, ElapsedUs(stamps[2], stamps[3], m_timestampFrequency));
            if (stamps[4] >= stamps[3])
                Accumulate(m_timings.readbackUs, ElapsedUs(stamps[3], stamps[4], m_timestampFrequency));
            slot.lastEndTimestamp = stamps[kQueriesPerSlot - 1];
        }
        LARGE_INTEGER t0{}, t1{}, frequency{};
        QueryPerformanceCounter(&t0);
        void* data = slot.mapped;
        const bool mapped = data || (dev->map_buffer_region(slot.buffer, 0, UINT64_MAX, map_access::read_only, &data) && data);
        if (mapped)
        {
            out.resize(static_cast<std::size_t>(m_frameBytes));
            m_copy.Copy(out.data(), data, static_cast<std::size_t>(m_frameBytes));
            if (!slot.mapped)
                dev->unmap_buffer_region(slot.buffer);
        }
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&frequency);
        Accumulate(m_timings.mapUs, static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart));
        slot.inFlight = false;
        slot.frame = -1;
        return mapped;
    }

    std::uint32_t GpuExporter::InFlight() const
    {
        std::uint32_t count = 0;
        for (const Slot& slot : m_slots)
            count += slot.inFlight ? 1u : 0u;
        return count;
    }

    bool GpuExporter::Sample(effect_runtime* runtime, float weight, bool first, bool last, std::int64_t frame, std::uint32_t seed,
        std::string& error)
    {
        if (!m_ready)
        {
            error = "not_ready";
            return false;
        }
        device* dev = runtime->get_device();
        command_list* cmd = runtime->get_command_queue()->get_immediate_command_list();
        const resource back = runtime->get_current_back_buffer();
        const resource_desc desc = dev->get_resource_desc(back);
        if (desc.texture.width != m_config.width || desc.texture.height != m_config.height)
        {
            error = "size_changed";
            return false;
        }

        // The slot this frame's readback goes to (only on the last sample).
        std::uint32_t slotIndex = ~0u;
        if (last)
        {
            LARGE_INTEGER w0{}, w1{}, frequency{};
            QueryPerformanceCounter(&w0);
            for (std::uint32_t i = 0; i < m_slots.size() && slotIndex == ~0u; ++i)
                if (!m_slots[i].inFlight)
                    slotIndex = i;
            if (slotIndex == ~0u)
            {
                // The caller drains first when Full() (the encoder takes the
                // oldest); never a flush from here.
                error = "ring_full";
                return false;
            }
            QueryPerformanceCounter(&w1);
            QueryPerformanceFrequency(&frequency);
            Accumulate(m_timings.ringWaitUs, static_cast<double>(w1.QuadPart - w0.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart));
        }
        const std::uint32_t q = slotIndex == ~0u ? ~0u : slotIndex * kQueriesPerSlot;

        // 1. Back buffer -> scratch.
        if (q != ~0u)
            cmd->end_query(m_queries, query_type::timestamp, q + 0);
        cmd->barrier(back, resource_usage::present, resource_usage::copy_source);
        cmd->barrier(m_scratch, resource_usage::shader_resource, resource_usage::copy_dest);
        cmd->copy_resource(back, m_scratch);
        cmd->barrier(m_scratch, resource_usage::copy_dest, resource_usage::shader_resource);
        cmd->barrier(back, resource_usage::copy_source, resource_usage::present);
        if (q != ~0u)
            cmd->end_query(m_queries, query_type::timestamp, q + 1);

        ExportConstants pc;
        pc.width = m_config.width;
        pc.height = m_config.height;
        pc.format = static_cast<std::uint32_t>(m_config.layout);
        pc.transfer = m_config.transfer;
        pc.barTop = m_config.barTop;
        pc.barBottom = m_config.barBottom;
        pc.grain = m_config.grain;
        pc.seed = seed;
        pc.weight = weight;

        auto bind = [&](pipeline which) {
            cmd->bind_pipeline(pipeline_stage::compute_shader, which);
            const resource_view srv = m_scratchSrv;
            const resource_view sum = m_sumUav;
            const resource_view output = m_outputUav;
            if (m_api == device_api::vulkan)
            {
                cmd->push_descriptors(shader_stage::compute, m_layout, 0,
                    descriptor_table_update{{}, 0, 0, 1, descriptor_type::texture_shader_resource_view, &srv});
                cmd->push_descriptors(shader_stage::compute, m_layout, 0, descriptor_table_update{{}, 1, 0, 1, descriptor_type::texture_unordered_access_view, &sum});
                cmd->push_descriptors(shader_stage::compute, m_layout, 0,
                    descriptor_table_update{{}, 2, 0, 1, descriptor_type::texture_unordered_access_view, &output});
            }
            else
            {
                const resource_view uavs[2] = {sum, output};
                cmd->push_descriptors(shader_stage::compute, m_layout, 0,
                    descriptor_table_update{{}, 0, 0, 1, descriptor_type::texture_shader_resource_view, &srv});
                cmd->push_descriptors(shader_stage::compute, m_layout, 1,
                    descriptor_table_update{{}, 0, 0, 2, descriptor_type::texture_unordered_access_view, uavs});
            }
            cmd->push_constants(shader_stage::compute, m_layout, m_constantsParam, 0, kConstantWords, &pc);
        };

        // 2. Accumulate (motion blur).
        if (m_config.accumulate)
        {
            pc.flags = first ? kFlagFirst : 0u;
            cmd->barrier(m_sum, resource_usage::unordered_access, resource_usage::unordered_access);
            bind(m_accumulate);
            cmd->dispatch((m_config.width + 7) / 8, (m_config.height + 7) / 8, 1);
        }
        if (q != ~0u)
            cmd->end_query(m_queries, query_type::timestamp, q + 2);
        if (!last)
            return true;

        // 3. Resolve into the output layout, then the staging slot.
        pc.flags = (m_config.accumulate ? kFlagFromSum : 0u) | (m_config.dither ? kFlagDither : 0u) | (m_config.bt2020 ? kFlagBt2020 : 0u);
        cmd->barrier(m_sum, resource_usage::unordered_access, resource_usage::unordered_access);
        bind(m_resolve);
        cmd->dispatch((m_config.width / 4 + 7) / 8, (m_config.height / 2 + 7) / 8, 1);
        cmd->end_query(m_queries, query_type::timestamp, q + 3);
        Slot& slot = m_slots[slotIndex];
        cmd->barrier(m_output, resource_usage::unordered_access, resource_usage::copy_source);
        if (!m_readbackFixedState)
            cmd->barrier(slot.buffer, resource_usage::cpu_access, resource_usage::copy_dest);
        // The whole image, rows tight: the frame's bytes in order from offset 0.
        cmd->copy_texture_to_buffer(m_output, 0, nullptr, slot.buffer, 0, kOutputRowWords, m_outputRows);
        if (!m_readbackFixedState)
            cmd->barrier(slot.buffer, resource_usage::copy_dest, resource_usage::cpu_access); // visible to the map
        cmd->barrier(m_output, resource_usage::copy_source, resource_usage::unordered_access);
        cmd->end_query(m_queries, query_type::timestamp, q + 4);
        slot.recordedPresent = m_present;
        slot.inFlight = true;
        slot.frame = frame;
        slot.order = m_sequence++;
        return true;
    }

    void GpuExporter::ReadTag(effect_runtime* runtime, std::uint64_t presentIndex)
    {
        if (!m_ready || m_tags.empty() || !m_tagQueries.handle)
            return;
        TagSlot& tag = m_tags[m_tagNext % m_tags.size()];
        if (tag.inFlight || !tag.buffer.handle)
            return; // a read still pending there: skip this picture
        const std::uint32_t index = m_tagNext % static_cast<std::uint32_t>(m_tags.size());
        ++m_tagNext;
        command_list* cmd = runtime->get_command_queue()->get_immediate_command_list();
        const resource back = runtime->get_current_back_buffer();
        cmd->barrier(back, resource_usage::present, resource_usage::copy_source);
        if (!m_readbackFixedState)
            cmd->barrier(tag.buffer, resource_usage::cpu_access, resource_usage::copy_dest);
        static_assert(kTagStride % kD3D12PlacementAlignment == 0, "tag cells on D3D12 placement alignment");
        for (std::uint32_t cell = 0; cell < replay::exportchannel::kTagCells; ++cell)
        {
            const std::uint32_t x = static_cast<std::uint32_t>(replay::exportchannel::TagCellCentreX(cell) * m_config.width);
            const std::uint32_t y = static_cast<std::uint32_t>(replay::exportchannel::TagCellCentreY() * m_config.height);
            const subresource_box box{x, y, 0, x + 1, y + 1, 1};
            cmd->copy_texture_to_buffer(back, 0, &box, tag.buffer, static_cast<std::uint64_t>(cell) * kTagStride, 0, 0);
        }
        if (!m_readbackFixedState)
            cmd->barrier(tag.buffer, resource_usage::copy_dest, resource_usage::cpu_access);
        cmd->barrier(back, resource_usage::copy_source, resource_usage::present);
        cmd->end_query(m_tagQueries, query_type::timestamp, index);
        tag.inFlight = true;
        tag.presentIndex = presentIndex;
    }

    void GpuExporter::End(device* dev)
    {
        if (!dev)
            dev = m_device;
        if (!dev)
            return;
        // The caller made sure the GPU is done with these (the ring drained,
        // or a wait_idle at shutdown).
        for (Slot& slot : m_slots)
        {
            if (slot.mapped)
                dev->unmap_buffer_region(slot.buffer);
            if (slot.buffer.handle)
                dev->destroy_resource(slot.buffer);
        }
        m_slots.clear();
        for (TagSlot& tag : m_tags)
            if (tag.buffer.handle)
                dev->destroy_resource(tag.buffer);
        m_tags.clear();
        if (m_queries.handle)
            dev->destroy_query_heap(m_queries);
        if (m_tagQueries.handle)
            dev->destroy_query_heap(m_tagQueries);
        if (m_accumulate.handle)
            dev->destroy_pipeline(m_accumulate);
        if (m_resolve.handle)
            dev->destroy_pipeline(m_resolve);
        if (m_layout.handle)
            dev->destroy_pipeline_layout(m_layout);
        if (m_scratchSrv.handle)
            dev->destroy_resource_view(m_scratchSrv);
        if (m_scratch.handle)
            dev->destroy_resource(m_scratch);
        if (m_sumUav.handle)
            dev->destroy_resource_view(m_sumUav);
        if (m_outputUav.handle)
            dev->destroy_resource_view(m_outputUav);
        if (m_sum.handle)
            dev->destroy_resource(m_sum);
        if (m_output.handle)
            dev->destroy_resource(m_output);
        m_copy.Stop();
        m_queries = {0};
        m_tagQueries = {0};
        m_accumulate = {0};
        m_resolve = {0};
        m_layout = {0};
        m_scratchSrv = {0};
        m_scratch = {0};
        m_sum = {0};
        m_output = {0};
        m_sumUav = {0};
        m_outputUav = {0};
        m_ready = false;
    }
}
