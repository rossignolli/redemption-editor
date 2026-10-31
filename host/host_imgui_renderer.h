#pragma once

// Draws ImGui through reshade::api (any API the host implements), as
// ReShade's own runtime does (source/runtime_gui.cpp, init_imgui_resources /
// render_imgui_draw_data / destroy_imgui_resources, BSD-3-Clause, Copyright
// (C) 2014 Patrick Mours). ImTextureID is a resource_view handle, so the
// core's texture views (thumbnails, art, fonts) draw as under ReShade; ImGui's
// own textures (the font atlas, ImGui 1.92 texture updates) are created here.
// One per device; the pipeline follows the back buffer format.

#include <imgui.h>
#include <reshade.hpp>

#include <cstddef>
#include <cstdint>

namespace host
{
    struct ShaderCode
    {
        const void* code = nullptr;
        size_t size = 0;
    };

    class ImGuiRenderer
    {
    public:
        // `rtFormat`: the render target view's format (the back buffer's non sRGB
        // variant). Again with another format rebuilds the pipeline only.
        bool Init(reshade::api::device* device, reshade::api::command_queue* queue, reshade::api::format rtFormat,
            ShaderCode vs, ShaderCode ps);
        // Texture updates, then the draw lists into `rtv` (in render_target state)
        // on `cmd` (the queue's immediate command list).
        void Render(reshade::api::command_list* cmd, ImDrawData* data, reshade::api::resource_view rtv);
        // Every resource, ImGui's textures in the current ImGui context too.
        void Shutdown();
        bool Ready() const { return m_pipeline.handle != 0; }

    private:
        void UpdateTextures(ImDrawData* data);

        reshade::api::device* m_device = nullptr;
        reshade::api::command_queue* m_queue = nullptr;
        reshade::api::format m_format = reshade::api::format::unknown;
        reshade::api::sampler m_sampler{0};
        reshade::api::pipeline_layout m_layout{0};
        reshade::api::pipeline m_pipeline{0};
        bool m_combined = true; // sampler_with_resource_view (Vulkan); else sampler + view (D3D12)
        // Multi buffered: the previous frames may still read theirs.
        static constexpr int kBuffers = 4;
        reshade::api::resource m_indices[kBuffers]{};
        reshade::api::resource m_vertices[kBuffers]{};
        int m_indexCount[kBuffers]{};
        int m_vertexCount[kBuffers]{};
        uint64_t m_frame = 0;
    };
}
