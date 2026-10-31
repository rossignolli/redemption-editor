#pragma once

// The editor's painted art: PNGs shipped next to RDR2.exe in
// RedemptionEditor\ui (like the title font in \fonts). The file list, sizes
// and slicing of each piece:
// Research: docs/research/editor-ui-art.md
// tools/ui_art/make_menu_art.py makes the row art and some menu icons.
//
// Loaded once on the render thread with the ReShade device, with a mip chain
// made here (the icons are drawn at 1/3 to 1/7 of their size at 1080 lines:
// without mips they shimmer; on Vulkan level 0 only, see ArtUploadLevels in
// editor_ui_qa.h), and released when the device goes. Decoded with
// WIC (stb_image is not available). A file that is missing or fails leaves
// its art out: the editor draws its flat look for it, logs one UI_ART line,
// never fails.

#include "bundled_files.h"
#include "editor_ui_qa.h" // ArtUploadLevels: no mips on Vulkan
#include "ui_probe.h"

#include <imgui.h>
#include <reshade.hpp>

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace ui_art
{
    enum class Art
    {
        EdgeInk,
        EdgeTorn,
        BarTorn,
        BarRed,
        BrushRed,
        IconKey,
        IconKeySelected,
        IconKeyTick,
        IconCut,
        IconShake,
        IconPlayhead,
        PanelGrain,
        IconCutSelected,
        IconShakeSelected,
        IconTrimIn,
        IconTrimOut,
        IconSeam,
        // The icons in pairs, gold then white (IconWhite).
        MenuClips,
        MenuClipsWhite,
        MenuProjects,
        MenuProjectsWhite,
        MenuRecord,
        MenuRecordWhite,
        MenuSettings,
        MenuSettingsWhite,
        MenuExport,
        MenuExportWhite,
        MenuNewProject,
        MenuNewProjectWhite,
        MenuSaveBuffer,
        MenuSaveBufferWhite,
        MenuEditClip,
        MenuEditClipWhite,
        MenuEditProject,
        MenuEditProjectWhite,
        MenuCloseProject,
        MenuCloseProjectWhite,
        ToolCamera,
        ToolCameraWhite,
        ToolDof,
        ToolDofWhite,
        ToolShake,
        ToolShakeWhite,
        ToolEffects,
        ToolEffectsWhite,
        ToolWeather,
        ToolWeatherWhite,
        Loading,
        ReelMark,
        Divider,
        Corner,
        Cursor,
        RowBox,
        RowInk,
        Count
    };

    // A menu or tool icon's white twin (drawn on the red brush).
    inline Art IconWhite(Art art) { return static_cast<Art>(static_cast<int>(art) + 1); }

    // The loading cue's frames, side by side in its sheet.
    constexpr int kLoadingFrames = 12;

    inline const wchar_t* FileName(Art art)
    {
        switch (art)
        {
        case Art::EdgeInk: return L"edge_ink.png";
        case Art::EdgeTorn: return L"edge_torn.png";
        case Art::BarTorn: return L"bar_torn.png";
        case Art::BarRed: return L"bar_red.png";
        case Art::BrushRed: return L"brush_red.png";
        case Art::IconKey: return L"icon_key.png";
        case Art::IconKeySelected: return L"icon_key_selected.png";
        case Art::IconKeyTick: return L"icon_key_tick.png";
        case Art::IconCut: return L"icon_cut.png";
        case Art::IconShake: return L"icon_shake.png";
        case Art::IconPlayhead: return L"icon_playhead.png";
        case Art::PanelGrain: return L"panel_grain.png";
        case Art::IconCutSelected: return L"icon_cut_selected.png";
        case Art::IconShakeSelected: return L"icon_shake_selected.png";
        case Art::IconTrimIn: return L"icon_trim_in.png";
        case Art::IconTrimOut: return L"icon_trim_out.png";
        case Art::IconSeam: return L"icon_seam.png";
        case Art::MenuClips: return L"menu_clips.png";
        case Art::MenuClipsWhite: return L"menu_clips_white.png";
        case Art::MenuProjects: return L"menu_projects.png";
        case Art::MenuProjectsWhite: return L"menu_projects_white.png";
        case Art::MenuRecord: return L"menu_record.png";
        case Art::MenuRecordWhite: return L"menu_record_white.png";
        case Art::MenuSettings: return L"menu_settings.png";
        case Art::MenuSettingsWhite: return L"menu_settings_white.png";
        case Art::MenuExport: return L"menu_export.png";
        case Art::MenuExportWhite: return L"menu_export_white.png";
        case Art::MenuNewProject: return L"menu_new_project.png";
        case Art::MenuNewProjectWhite: return L"menu_new_project_white.png";
        case Art::MenuSaveBuffer: return L"menu_save_buffer.png";
        case Art::MenuSaveBufferWhite: return L"menu_save_buffer_white.png";
        case Art::MenuEditClip: return L"menu_edit_clip.png";
        case Art::MenuEditClipWhite: return L"menu_edit_clip_white.png";
        case Art::MenuEditProject: return L"menu_edit_project.png";
        case Art::MenuEditProjectWhite: return L"menu_edit_project_white.png";
        case Art::MenuCloseProject: return L"menu_close_project.png";
        case Art::MenuCloseProjectWhite: return L"menu_close_project_white.png";
        case Art::ToolCamera: return L"tool_camera.png";
        case Art::ToolCameraWhite: return L"tool_camera_white.png";
        case Art::ToolDof: return L"tool_dof.png";
        case Art::ToolDofWhite: return L"tool_dof_white.png";
        case Art::ToolShake: return L"tool_shake.png";
        case Art::ToolShakeWhite: return L"tool_shake_white.png";
        case Art::ToolEffects: return L"tool_effects.png";
        case Art::ToolEffectsWhite: return L"tool_effects_white.png";
        case Art::ToolWeather: return L"tool_weather.png";
        case Art::ToolWeatherWhite: return L"tool_weather_white.png";
        case Art::Loading: return L"loading_cylinder.png";
        case Art::ReelMark: return L"reel_mark.png";
        case Art::Divider: return L"divider.png";
        case Art::Corner: return L"corner_star.png";
        case Art::Cursor: return L"cursor_ink.png";
        case Art::RowBox: return L"row_box.png";
        case Art::RowInk: return L"row_ink.png";
        default: return L"";
        }
    }

    struct Texture
    {
        reshade::api::resource resource{0};
        reshade::api::resource_view view{0};
        ImU64 id = 0;
        int width = 0;
        int height = 0;
    };

    struct State
    {
        bool tried = false;
        reshade::api::device* device = nullptr;
        Texture textures[static_cast<int>(Art::Count)];
    };
    inline State g_state;

    // A PNG to straight (not premultiplied) RGBA8, top row first.
    inline bool DecodePng(const std::wstring& path, std::vector<std::uint32_t>& rgba, int& width, int& height)
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* factory = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        UINT w = 0, h = 0;
        bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(converter->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 4096 && h <= 4096;
        if (ok)
        {
            rgba.assign(static_cast<std::size_t>(w) * h, 0);
            ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(rgba.data())));
        }
        if (converter)
            converter->Release();
        if (frame)
            frame->Release();
        if (decoder)
            decoder->Release();
        if (factory)
            factory->Release();
        if (co == S_OK || co == S_FALSE)
            CoUninitialize();
        width = static_cast<int>(w);
        height = static_cast<int>(h);
        return ok;
    }

    // The next mip level: 2x2 boxes, the colour weighted by alpha (the
    // transparent texels' colour never bleeds into an icon's edge).
    inline std::vector<std::uint32_t> HalfSize(const std::vector<std::uint32_t>& rgba, int width, int height, int& outW, int& outH)
    {
        outW = width > 1 ? width / 2 : 1;
        outH = height > 1 ? height / 2 : 1;
        std::vector<std::uint32_t> out(static_cast<std::size_t>(outW) * outH);
        for (int y = 0; y < outH; ++y)
            for (int x = 0; x < outW; ++x)
            {
                double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
                int n = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const int sx = std::min(x * 2 + dx, width - 1), sy = std::min(y * 2 + dy, height - 1);
                        const std::uint32_t p = rgba[static_cast<std::size_t>(sy) * width + sx];
                        const double pa = static_cast<double>(p >> 24);
                        r += (p & 0xFF) * pa;
                        g += ((p >> 8) & 0xFF) * pa;
                        b += ((p >> 16) & 0xFF) * pa;
                        a += pa;
                        ++n;
                    }
                const auto channel = [a](double sum) { return a > 0.0 ? static_cast<std::uint32_t>(sum / a + 0.5) : 0u; };
                const std::uint32_t alpha = static_cast<std::uint32_t>(a / n + 0.5);
                out[static_cast<std::size_t>(y) * outW + x] = channel(r) | (channel(g) << 8) | (channel(b) << 16) | (alpha << 24);
            }
        return out;
    }

    inline void DestroyTexture(Texture& texture)
    {
        if (g_state.device)
        {
            if (texture.view.handle)
                g_state.device->destroy_resource_view(texture.view);
            if (texture.resource.handle)
                g_state.device->destroy_resource(texture.resource);
        }
        texture = Texture{};
    }

    // Once per device, on the render thread; one UI_ART line in ReShade's log.
    inline void Ensure(reshade::api::effect_runtime* runtime)
    {
        using namespace reshade::api;
        State& s = g_state;
        if (s.tried || !runtime)
            return;
        s.tried = true;
        s.device = runtime->get_device();
        const std::wstring folder = bundled_files::RedemptionEditorFolder() + L"ui\\";
        int loaded = 0;
        std::uint64_t bytes = 0; // the loaded textures with their mips
        std::string missing;
        for (int n = 0; n < static_cast<int>(Art::Count); ++n)
        {
            const Art art = static_cast<Art>(n);
            if (art == Art::EdgeTorn || art == Art::PanelGrain)
                continue; // shipped, not drawn
            Texture& texture = s.textures[n];
            std::vector<std::uint32_t> rgba;
            int width = 0, height = 0;
            bool ok = DecodePng(folder + FileName(art), rgba, width, height);
            if (ok)
            {
                // The mip chain, every level kept until the texture is made.
                std::vector<std::vector<std::uint32_t>> levels;
                std::vector<int> widths, heights;
                levels.push_back(std::move(rgba));
                widths.push_back(width);
                heights.push_back(height);
                while (widths.back() > 1 || heights.back() > 1)
                {
                    int w = 0, h = 0;
                    levels.push_back(HalfSize(levels.back(), widths.back(), heights.back(), w, h));
                    widths.push_back(w);
                    heights.push_back(h);
                }
                std::size_t uploaded = levels.size(); // the levels the texture holds
#ifdef REDEMPTION_UI_HARNESS
                if (ui_probe::Current())
                {
                    std::vector<const void*> data;
                    for (const auto& level : levels)
                        data.push_back(level.data());
                    texture.id = ui_probe::Current()->CreateTextureMips(data.data(), width, height, static_cast<int>(levels.size()));
                    ok = texture.id != 0;
                }
                else
#endif
                {
                    ok = s.device != nullptr;
                    if (ok)
                    {
                        // Vulkan: level 0 only (ReShade's Vulkan upload reads past the smaller levels).
                        uploaded = replay::editor::uiqa::ArtUploadLevels(s.device->get_api() == device_api::vulkan, levels.size());
                        std::vector<subresource_data> data(uploaded);
                        for (std::size_t l = 0; l < uploaded; ++l)
                        {
                            data[l].data = levels[l].data();
                            data[l].row_pitch = static_cast<std::uint32_t>(widths[l]) * 4;
                            data[l].slice_pitch = data[l].row_pitch * static_cast<std::uint32_t>(heights[l]);
                        }
                        const resource_desc desc(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1,
                            static_cast<std::uint16_t>(uploaded), format::r8g8b8a8_unorm, 1, memory_heap::gpu_only, resource_usage::shader_resource);
                        ok = s.device->create_resource(desc, data.data(), resource_usage::shader_resource, &texture.resource) &&
                            s.device->create_resource_view(texture.resource, resource_usage::shader_resource,
                                resource_view_desc(format::r8g8b8a8_unorm), &texture.view);
                        if (ok)
                            texture.id = static_cast<ImU64>(texture.view.handle);
                        else
                            DestroyTexture(texture);
                    }
                }
                texture.width = width;
                texture.height = height;
                if (ok)
                    for (std::size_t l = 0; l < uploaded; ++l)
                        bytes += static_cast<std::uint64_t>(widths[l]) * heights[l] * 4;
            }
            if (ok)
                ++loaded;
            else
            {
                char name[64];
                std::snprintf(name, sizeof(name), "%ls", FileName(art));
                missing += (missing.empty() ? "" : ";") + std::string(name);
            }
        }
        char line[2048];
        std::snprintf(line, sizeof(line), "UI_ART,loaded=%d,kb=%llu,missing=%s,folder=%ls", loaded, static_cast<unsigned long long>(bytes / 1024),
            missing.empty() ? "none" : missing.c_str(), folder.c_str());
        reshade::log::message(missing.empty() ? reshade::log::level::info : reshade::log::level::warning, line);
    }

    // 0 when the art is missing (the caller draws its flat look).
    inline ImTextureID Get(Art art) { return static_cast<ImTextureID>(g_state.textures[static_cast<int>(art)].id); }
    inline const Texture& Info(Art art) { return g_state.textures[static_cast<int>(art)]; }
    inline float Aspect(Art art)
    {
        const Texture& t = g_state.textures[static_cast<int>(art)];
        return t.height > 0 ? static_cast<float>(t.width) / static_cast<float>(t.height) : 1.0f;
    }

    inline void Release()
    {
        State& s = g_state;
        for (Texture& texture : s.textures)
        {
#ifdef REDEMPTION_UI_HARNESS
            if (!texture.resource.handle)
            {
                texture = Texture{}; // the harness owns its textures
                continue;
            }
#endif
            DestroyTexture(texture);
        }
        s.device = nullptr;
        s.tried = false;
    }
}
