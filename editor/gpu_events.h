#pragma once

// GPU events forwarded by the loader to the core. ReShade delivers application
// events (resources, descriptors, dispatches, copies) only to a registered
// add-on, and registering them while the game renders is a race: the loader
// registers them once at startup, only with [RedemptionEditor] EditorGpuEvents=1
// (off by default: without it ReShade does not even build the event arguments),
// and forwards each one to the table the core exports (EditorCoreGpuEvents).
// Every entry may be null. The loader counts the calls in flight and swaps the
// core only when none is. No consumer exists now; the core exports no table.

#include <reshade.hpp>

namespace replay::editor::gpu
{
    using namespace reshade::api;

    struct EventTable
    {
        // May widen the usage of a texture it wants to copy (return true).
        bool (*createResource)(device*, resource_desc&, subresource_data*, resource_usage) = nullptr;
        void (*initResource)(device*, const resource_desc&, const subresource_data*, resource_usage, resource) = nullptr;
        void (*destroyResource)(device*, resource) = nullptr;
        void (*initResourceView)(device*, resource, resource_usage, const resource_view_desc&, resource_view) = nullptr;
        void (*destroyResourceView)(device*, resource_view) = nullptr;
        bool (*updateDescriptorTables)(device*, uint32_t, const descriptor_table_update*) = nullptr;
        void (*bindDescriptorTables)(command_list*, shader_stage, pipeline_layout, uint32_t, uint32_t, const descriptor_table*) = nullptr;
        void (*bindPipeline)(command_list*, pipeline_stage, pipeline) = nullptr;
        bool (*dispatch)(command_list*, uint32_t, uint32_t, uint32_t) = nullptr;
        bool (*copyTextureToBuffer)(command_list*, resource, uint32_t, const subresource_box*, resource, uint64_t, uint32_t, uint32_t) = nullptr;
        bool (*copyTextureRegion)(command_list*, resource, uint32_t, const subresource_box*, resource, uint32_t, const subresource_box*, filter_mode) = nullptr;
        bool (*copyResource)(command_list*, resource, resource) = nullptr;
        void (*barrier)(command_list*, uint32_t, const resource*, const resource_usage*, const resource_usage*) = nullptr;
        void (*executeCommandList)(command_queue*, command_list*) = nullptr;
        void (*present)(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) = nullptr;
    };

    using GetTableFn = const EventTable* (*)();
}
