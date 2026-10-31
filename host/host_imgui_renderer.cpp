// ImGui through reshade::api (host_imgui_renderer.h). Ported from ReShade's
// source/runtime_gui.cpp (BSD-3-Clause, Copyright (C) 2014 Patrick Mours):
// the same pipeline, push constants and texture handling, without the VR,
// OpenGL and D3D9 branches.

#include "host_imgui_renderer.h"
#include "host_log.h"

#include <cstring>
#include <vector>

namespace host
{
    using namespace reshade::api;

    bool ImGuiRenderer::Init(device* dev, command_queue* queue, format rtFormat, ShaderCode vs, ShaderCode ps)
    {
        if (m_device && m_device != dev)
            Shutdown();
        m_device = dev;
        m_queue = queue;
        m_combined = dev->check_capability(device_caps::sampler_with_resource_view);

        if (m_sampler.handle == 0)
        {
            sampler_desc desc = {};
            desc.filter = filter_mode::min_mag_mip_linear;
            desc.address_u = texture_address_mode::clamp;
            desc.address_v = texture_address_mode::clamp;
            desc.address_w = texture_address_mode::clamp;
            if (!dev->create_sampler(desc, &m_sampler))
            {
                HOST_ERROR("HOST_IMGUI failed reason=create_sampler");
                return false;
            }
        }
        if (m_layout.handle == 0)
        {
            uint32_t count = 0;
            pipeline_layout_param params[3];
            if (m_combined)
                params[count++] = descriptor_range{0, 0, 0, 1, shader_stage::pixel, 1, descriptor_type::sampler_with_resource_view};
            else
            {
                params[count++] = descriptor_range{0, 0, 0, 1, shader_stage::pixel, 1, descriptor_type::sampler};
                params[count++] = descriptor_range{0, 0, 0, 1, shader_stage::pixel, 1, descriptor_type::shader_resource_view};
            }
            params[count++] = constant_range{0, 0, 0, 18, shader_stage::vertex | shader_stage::pixel};
            if (!dev->create_pipeline_layout(count, params, &m_layout))
            {
                HOST_ERROR("HOST_IMGUI failed reason=create_pipeline_layout");
                return false;
            }
        }
        if (m_pipeline.handle != 0 && m_format == rtFormat)
            return true;
        if (m_pipeline.handle != 0)
        {
            queue->wait_idle();
            dev->destroy_pipeline(m_pipeline);
            m_pipeline = {0};
        }

        shader_desc vsDesc;
        vsDesc.code = vs.code;
        vsDesc.code_size = vs.size;
        shader_desc psDesc;
        psDesc.code = ps.code;
        psDesc.code_size = ps.size;

        std::vector<pipeline_subobject> subobjects;
        subobjects.push_back({pipeline_subobject_type::vertex_shader, 1, &vsDesc});
        subobjects.push_back({pipeline_subobject_type::pixel_shader, 1, &psDesc});
        const input_element inputLayout[3] = {
            {0, "POSITION", 0, format::r32g32_float, 0, offsetof(ImDrawVert, pos), sizeof(ImDrawVert), 0},
            {1, "TEXCOORD", 0, format::r32g32_float, 0, offsetof(ImDrawVert, uv), sizeof(ImDrawVert), 0},
            {2, "COLOR", 0, format::r8g8b8a8_unorm, 0, offsetof(ImDrawVert, col), sizeof(ImDrawVert), 0},
        };
        subobjects.push_back({pipeline_subobject_type::input_layout, 3, const_cast<input_element*>(inputLayout)});
        primitive_topology topology = primitive_topology::triangle_list;
        subobjects.push_back({pipeline_subobject_type::primitive_topology, 1, &topology});
        blend_desc blend;
        blend.blend_enable[0] = true;
        blend.source_color_blend_factor[0] = blend_factor::source_alpha;
        blend.dest_color_blend_factor[0] = blend_factor::one_minus_source_alpha;
        blend.color_blend_op[0] = blend_op::add;
        blend.source_alpha_blend_factor[0] = blend_factor::one;
        blend.dest_alpha_blend_factor[0] = blend_factor::one_minus_source_alpha;
        blend.alpha_blend_op[0] = blend_op::add;
        blend.render_target_write_mask[0] = 0xF;
        subobjects.push_back({pipeline_subobject_type::blend_state, 1, &blend});
        rasterizer_desc rasterizer;
        rasterizer.cull_mode = cull_mode::none;
        rasterizer.scissor_enable = true;
        subobjects.push_back({pipeline_subobject_type::rasterizer_state, 1, &rasterizer});
        depth_stencil_desc depthStencil;
        depthStencil.depth_enable = false;
        depthStencil.stencil_enable = false;
        subobjects.push_back({pipeline_subobject_type::depth_stencil_state, 1, &depthStencil});
        format target = rtFormat;
        subobjects.push_back({pipeline_subobject_type::render_target_formats, 1, &target});
        if (!dev->create_pipeline(m_layout, static_cast<uint32_t>(subobjects.size()), subobjects.data(), &m_pipeline))
        {
            HOST_ERROR("HOST_IMGUI failed reason=create_pipeline format=%u", static_cast<unsigned>(rtFormat));
            m_pipeline = {0};
            return false;
        }
        m_format = rtFormat;
        return true;
    }

    void ImGuiRenderer::UpdateTextures(ImDrawData* data)
    {
        if (!data->Textures)
            return;
        command_list* const cmd = m_queue->get_immediate_command_list();
        for (ImTextureData* const texture : *data->Textures)
        {
            if (texture->Status == ImTextureStatus_OK)
                continue;
            if (texture->Status == ImTextureStatus_WantCreate)
            {
                format fmt = format::unknown;
                switch (texture->Format)
                {
                case ImTextureFormat_RGBA32:
                    fmt = format::r8g8b8a8_unorm;
                    break;
                case ImTextureFormat_Alpha8:
                    fmt = format::r8_unorm;
                    break;
                }
                const subresource_data initial = {texture->GetPixels(), static_cast<uint32_t>(texture->GetPitch()),
                    static_cast<uint32_t>(texture->GetSizeInBytes())};
                resource tex{0};
                if (!m_device->create_resource(resource_desc(texture->Width, texture->Height, 1, 1, fmt, 1, memory_heap::gpu_only,
                        resource_usage::shader_resource | resource_usage::copy_dest), &initial, resource_usage::shader_resource, &tex))
                {
                    HOST_ERROR("HOST_IMGUI texture create failed %dx%d", texture->Width, texture->Height);
                    texture->SetStatus(ImTextureStatus_Destroyed);
                    continue;
                }
                resource_view srv{0};
                if (!m_device->create_resource_view(tex, resource_usage::shader_resource, resource_view_desc(fmt), &srv))
                {
                    HOST_ERROR("HOST_IMGUI texture view failed");
                    m_device->destroy_resource(tex);
                    texture->SetStatus(ImTextureStatus_Destroyed);
                    continue;
                }
                texture->SetTexID(srv.handle);
                texture->SetStatus(ImTextureStatus_OK);
                continue;
            }
            if (texture->Status == ImTextureStatus_WantUpdates)
            {
                const resource_view srv = {texture->GetTexID()};
                const resource tex = m_device->get_resource_from_view(srv);
                cmd->barrier(tex, resource_usage::shader_resource, resource_usage::copy_dest);
                for (const ImTextureRect& rect : texture->Updates)
                {
                    subresource_box box;
                    box.left = rect.x;
                    box.top = rect.y;
                    box.front = 0;
                    box.right = static_cast<uint32_t>(rect.x + rect.w);
                    box.bottom = static_cast<uint32_t>(rect.y + rect.h);
                    box.back = 1;
                    m_device->update_texture_region(subresource_data{texture->GetPixelsAt(rect.x, rect.y), static_cast<uint32_t>(texture->GetPitch()),
                        static_cast<uint32_t>(texture->GetSizeInBytes())}, tex, 0, &box);
                }
                cmd->barrier(tex, resource_usage::copy_dest, resource_usage::shader_resource);
                texture->SetStatus(ImTextureStatus_OK);
                continue;
            }
            if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames > 8)
            {
                const resource_view srv = {texture->GetTexID()};
                const resource tex = m_device->get_resource_from_view(srv);
                m_device->destroy_resource_view(srv);
                m_device->destroy_resource(tex);
                texture->SetTexID(ImTextureID_Invalid);
                texture->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    void ImGuiRenderer::Render(command_list* cmd, ImDrawData* data, resource_view rtv)
    {
        if (!data || !m_pipeline.handle)
            return;
        UpdateTextures(data);
        if (data->TotalVtxCount <= 0 || data->CmdListsCount <= 0)
            return;

        const int index = static_cast<int>(m_frame++ % kBuffers);
        if (m_indexCount[index] < data->TotalIdxCount)
        {
            if (m_indices[index].handle)
            {
                m_queue->wait_idle(); // nothing may still read the old one
                m_device->destroy_resource(m_indices[index]);
                m_indices[index] = {0};
            }
            const int size = data->TotalIdxCount + 10000;
            if (!m_device->create_resource(resource_desc(size * sizeof(ImDrawIdx), memory_heap::cpu_to_gpu, resource_usage::index_buffer), nullptr,
                    resource_usage::cpu_access, &m_indices[index]))
            {
                HOST_ERROR("HOST_IMGUI index buffer failed");
                m_indexCount[index] = 0;
                return;
            }
            m_indexCount[index] = size;
        }
        if (m_vertexCount[index] < data->TotalVtxCount)
        {
            if (m_vertices[index].handle)
            {
                m_queue->wait_idle();
                m_device->destroy_resource(m_vertices[index]);
                m_vertices[index] = {0};
            }
            const int size = data->TotalVtxCount + 5000;
            if (!m_device->create_resource(resource_desc(size * sizeof(ImDrawVert), memory_heap::cpu_to_gpu, resource_usage::vertex_buffer), nullptr,
                    resource_usage::cpu_access, &m_vertices[index]))
            {
                HOST_ERROR("HOST_IMGUI vertex buffer failed");
                m_vertexCount[index] = 0;
                return;
            }
            m_vertexCount[index] = size;
        }

        ImDrawIdx* indexOut = nullptr;
        if (m_device->map_buffer_region(m_indices[index], 0, UINT64_MAX, map_access::write_only, reinterpret_cast<void**>(&indexOut)))
        {
            for (int n = 0; n < data->CmdListsCount; ++n)
            {
                const ImDrawList* list = data->CmdLists[n];
                std::memcpy(indexOut, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
                indexOut += list->IdxBuffer.Size;
            }
            m_device->unmap_buffer_region(m_indices[index]);
        }
        ImDrawVert* vertexOut = nullptr;
        if (m_device->map_buffer_region(m_vertices[index], 0, UINT64_MAX, map_access::write_only, reinterpret_cast<void**>(&vertexOut)))
        {
            for (int n = 0; n < data->CmdListsCount; ++n)
            {
                const ImDrawList* list = data->CmdLists[n];
                std::memcpy(vertexOut, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
                vertexOut += list->VtxBuffer.Size;
            }
            m_device->unmap_buffer_region(m_vertices[index]);
        }

        render_pass_render_target_desc target = {};
        target.view = rtv;
        cmd->begin_render_pass(1, &target, nullptr);
        cmd->bind_pipeline(pipeline_stage::all_graphics, m_pipeline);
        cmd->bind_index_buffer(m_indices[index], 0, sizeof(ImDrawIdx));
        cmd->bind_vertex_buffer(0, m_vertices[index], 0, sizeof(ImDrawVert));
        const viewport view = {0, 0, data->DisplaySize.x, data->DisplaySize.y, 0.0f, 1.0f};
        cmd->bind_viewports(0, 1, &view);

        // D3D style clip space (Vulkan and D3D12 alike in ReShade's API), depth 0..1.
        const struct
        {
            float projection[16];
            color_space space;
            float brightness;
        } constants = {
            {
                2.0f / data->DisplaySize.x, 0.0f, 0.0f, 0.0f,
                0.0f, -2.0f / data->DisplaySize.y, 0.0f, 0.0f,
                0.0f, 0.0f, 0.5f, 0.0f,
                -(2 * data->DisplayPos.x + data->DisplaySize.x) / data->DisplaySize.x,
                (2 * data->DisplayPos.y + data->DisplaySize.y) / data->DisplaySize.y, 0.5f, 1.0f,
            },
            color_space::srgb_nonlinear,
            203.0f,
        };
        cmd->push_constants(shader_stage::vertex | shader_stage::pixel, m_layout, m_combined ? 1 : 2, 0, sizeof(constants) / 4, &constants);
        if (!m_combined)
            cmd->push_descriptors(shader_stage::pixel, m_layout, 0, descriptor_table_update{{}, 0, 0, 1, descriptor_type::sampler, &m_sampler});

        int vertexOffset = 0, indexOffset = 0;
        for (int n = 0; n < data->CmdListsCount; ++n)
        {
            const ImDrawList* list = data->CmdLists[n];
            for (const ImDrawCmd& draw : list->CmdBuffer)
            {
                if (draw.UserCallback != nullptr)
                {
                    draw.UserCallback(list, &draw);
                    continue;
                }
                const rect scissor = {
                    static_cast<int32_t>(draw.ClipRect.x - data->DisplayPos.x),
                    static_cast<int32_t>(draw.ClipRect.y - data->DisplayPos.y),
                    static_cast<int32_t>(draw.ClipRect.z - data->DisplayPos.x),
                    static_cast<int32_t>(draw.ClipRect.w - data->DisplayPos.y),
                };
                if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
                    continue;
                cmd->bind_scissor_rects(0, 1, &scissor);
                const resource_view srv = {draw.GetTexID()};
                if (m_combined)
                {
                    sampler_with_resource_view both = {m_sampler, srv};
                    cmd->push_descriptors(shader_stage::pixel, m_layout, 0,
                        descriptor_table_update{{}, 0, 0, 1, descriptor_type::sampler_with_resource_view, &both});
                }
                else
                    cmd->push_descriptors(shader_stage::pixel, m_layout, 1, descriptor_table_update{{}, 0, 0, 1, descriptor_type::shader_resource_view, &srv});
                cmd->draw_indexed(draw.ElemCount, 1, draw.IdxOffset + indexOffset, static_cast<int32_t>(draw.VtxOffset) + vertexOffset, 0);
            }
            indexOffset += list->IdxBuffer.Size;
            vertexOffset += list->VtxBuffer.Size;
        }
        cmd->end_render_pass();
    }

    void ImGuiRenderer::Shutdown()
    {
        if (!m_device)
            return;
        m_queue->wait_idle();
        if (ImGui::GetCurrentContext())
        {
            for (ImTextureData* const texture : ImGui::GetPlatformIO().Textures)
            {
                if (texture->Status == ImTextureStatus_Destroyed || texture->GetTexID() == ImTextureID_Invalid)
                    continue;
                const resource_view srv = {texture->GetTexID()};
                const resource tex = m_device->get_resource_from_view(srv);
                m_device->destroy_resource_view(srv);
                m_device->destroy_resource(tex);
                texture->SetTexID(ImTextureID_Invalid);
                // ImGui asks for it again (WantCreate) on its next frame: the atlas
                // keeps the pixels (imgui_draw.cpp, "Destroy was done by backend").
                texture->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        for (int i = 0; i < kBuffers; ++i)
        {
            if (m_indices[i].handle)
                m_device->destroy_resource(m_indices[i]);
            if (m_vertices[i].handle)
                m_device->destroy_resource(m_vertices[i]);
            m_indices[i] = m_vertices[i] = {0};
            m_indexCount[i] = m_vertexCount[i] = 0;
        }
        if (m_pipeline.handle)
            m_device->destroy_pipeline(m_pipeline);
        if (m_layout.handle)
            m_device->destroy_pipeline_layout(m_layout);
        if (m_sampler.handle)
            m_device->destroy_sampler(m_sampler);
        m_pipeline = {0};
        m_layout = {0};
        m_sampler = {0};
        m_device = nullptr;
        m_queue = nullptr;
        m_format = format::unknown;
    }
}
