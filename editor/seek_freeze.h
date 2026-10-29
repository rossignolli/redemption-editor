#pragma once

// The far jump's frozen picture. On a far jump the last picture stays on
// screen with a spinner until the world and the entities at the target
// are ready. The RDR2 script cannot pause the renderer, so the add-on does
// it: while a clip plays the back buffer is copied every frame into a
// texture of its own (in the overlay callback, before the editor's UI is
// drawn on it), and while the core reports SeekLoadFrozen the copying
// stops and that texture is drawn over the whole screen, under the
// editor's UI, with a spinner.
//
// The core decides "far" in the jump's own tick and publishes the snapshot
// at the end of that tick, before the frame showing the jump renders; the
// last copy taken before the editor reads the frozen state is therefore a
// picture from before the jump (the SEEK_FREEZE lines in ReShade.log show
// it). A near settle that lasts shows the spinner alone (the picture stays
// live).
// Render thread only (the overlay callback).

#include <imgui.h>
#include <reshade.hpp>

#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace seek_freeze
{
    constexpr std::uint32_t kSettlingSpinnerMs = 250; // a near settle shorter than this shows nothing

    struct State
    {
        reshade::api::device* device = nullptr;
        reshade::api::resource texture{0};
        reshade::api::resource_view view{0};
        reshade::api::resource_desc desc{};
        bool captured = false;     // the texture holds a picture
        bool frozen = false;       // drawing it now
        std::uint32_t sequence = 0; // the core's seekLoadSequence being shown
        ULONGLONG frozenAt = 0;
        std::uint32_t frozenFrames = 0;
        std::uint32_t copies = 0;
        std::uint32_t failures = 0;
        std::uint32_t logs = 0;
    };

    inline State& Get()
    {
        static State state;
        return state;
    }

    inline void Log(const char* line)
    {
        State& s = Get();
        if (s.logs >= 200)
            return;
        ++s.logs;
        reshade::log::message(reshade::log::level::info, line);
    }

    inline void DestroyTexture()
    {
        State& s = Get();
        if (s.device)
        {
            if (s.view.handle)
                s.device->destroy_resource_view(s.view);
            if (s.texture.handle)
                s.device->destroy_resource(s.texture);
        }
        s.view = {0};
        s.texture = {0};
        s.captured = false;
    }

    inline void Release()
    {
        DestroyTexture();
        State& s = Get();
        s.device = nullptr;
        s.frozen = false;
    }

    // The texture matching the back buffer (made again when its size or
    // format changes).
    inline bool EnsureTexture(reshade::api::device* dev, const reshade::api::resource_desc& back)
    {
        using namespace reshade::api;
        State& s = Get();
        if (s.device != dev)
        {
            DestroyTexture();
            s.device = dev;
        }
        if (s.texture.handle && s.desc.texture.width == back.texture.width && s.desc.texture.height == back.texture.height &&
            s.desc.texture.format == back.texture.format)
            return true;
        DestroyTexture();
        if (back.type != resource_type::texture_2d || back.texture.samples > 1)
            return false;
        const format typeless = format_to_typeless(back.texture.format);
        const resource_desc desc(back.texture.width, back.texture.height, 1, 1, typeless, 1, memory_heap::gpu_only,
            resource_usage::copy_dest | resource_usage::shader_resource);
        if (!dev->create_resource(desc, nullptr, resource_usage::shader_resource, &s.texture))
        {
            s.texture = {0};
            return false;
        }
        const format viewFormat = format_to_default_typed(back.texture.format);
        if (!dev->create_resource_view(s.texture, resource_usage::shader_resource, resource_view_desc(viewFormat), &s.view))
        {
            dev->destroy_resource(s.texture);
            s.texture = {0};
            s.view = {0};
            return false;
        }
        s.desc = back;
        char line[160];
        std::snprintf(line, sizeof(line), "SEEK_FREEZE,phase=texture,width=%u,height=%u,format=%u,view_format=%u",
            back.texture.width, back.texture.height, static_cast<unsigned>(back.texture.format),
            static_cast<unsigned>(viewFormat));
        Log(line);
        return true;
    }

    // The back buffer as it is now (the game's picture, before the editor's
    // UI), into the texture. GPU to GPU, no wait.
    inline void Capture(reshade::api::effect_runtime* runtime)
    {
        using namespace reshade::api;
        State& s = Get();
        device* dev = runtime->get_device();
        const resource back = runtime->get_current_back_buffer();
        if (!back.handle)
            return;
        const resource_desc desc = dev->get_resource_desc(back);
        if (!EnsureTexture(dev, desc))
        {
            if (++s.failures == 1)
                Log("SEEK_FREEZE,phase=texture,status=create_failed");
            return;
        }
        command_list* cmd = runtime->get_command_queue()->get_immediate_command_list();
        cmd->barrier(back, resource_usage::present, resource_usage::copy_source);
        cmd->barrier(s.texture, resource_usage::shader_resource, resource_usage::copy_dest);
        cmd->copy_resource(back, s.texture);
        cmd->barrier(s.texture, resource_usage::copy_dest, resource_usage::shader_resource);
        cmd->barrier(back, resource_usage::copy_source, resource_usage::present);
        s.captured = true;
        ++s.copies;
    }

    // A ring of eight dots, the lit one going round, bottom right. `uiScale`:
    // the editor's interface scale (src/ui_scale.h Effective).
    inline void DrawSpinner(ImDrawList* draw, const ImVec2& size, ULONGLONG now, float uiScale)
    {
        const float scale = size.y / 1080.0f * uiScale;
        const ImVec2 centre(size.x - 64.0f * scale, size.y - 64.0f * scale);
        const float radius = 18.0f * scale;
        constexpr int kDots = 8;
        const int lit = static_cast<int>((now / 90) % kDots);
        for (int dot = 0; dot < kDots; ++dot)
        {
            const float angle = 6.2831853f * static_cast<float>(dot) / kDots - 1.5707963f;
            const int age = (lit - dot + kDots) % kDots;
            const int alpha = 255 - age * 28;
            draw->AddCircleFilled(ImVec2(centre.x + std::cos(angle) * radius, centre.y + std::sin(angle) * radius),
                (age == 0 ? 4.0f : 3.0f) * scale, IM_COL32(255, 255, 255, alpha > 40 ? alpha : 40), 12);
        }
    }

    // Once per overlay frame, before the editor draws. `state` is the
    // snapshot's seekLoadState (0 none, 1 settling, 2 frozen). `spinner`
    // false: the frozen picture without the spinner (H hid the replay UI).
    // `uiScale`: the editor's interface scale, the spinner's size.
    inline void Update(reshade::api::effect_runtime* runtime, bool playing, std::uint32_t state, std::uint32_t sequence,
        std::uint32_t elapsedMs, bool spinner = true, float uiScale = 1.0f)
    {
        State& s = Get();
        const ULONGLONG now = GetTickCount64();
        const bool frozen = playing && state == 2;
        if (frozen && !s.frozen)
        {
            s.frozen = true;
            s.sequence = sequence;
            s.frozenAt = now;
            s.frozenFrames = 0;
            char line[160];
            std::snprintf(line, sizeof(line), "SEEK_FREEZE,phase=show,sequence=%u,captured=%d,copies=%u,core_elapsed_ms=%u",
                sequence, s.captured ? 1 : 0, s.copies, elapsedMs);
            Log(line);
        }
        else if (!frozen && s.frozen)
        {
            s.frozen = false;
            char line[160];
            std::snprintf(line, sizeof(line), "SEEK_FREEZE,phase=hide,sequence=%u,frames=%u,ms=%llu", s.sequence, s.frozenFrames,
                static_cast<unsigned long long>(now - s.frozenAt));
            Log(line);
        }
        if (!playing)
        {
            s.captured = false; // a picture of another clip is never shown
            return;
        }
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        if (s.frozen)
        {
            ++s.frozenFrames;
            if (s.captured && s.view.handle)
                ImGui::GetBackgroundDrawList()->AddImage(ImTextureRef(static_cast<ImTextureID>(s.view.handle)), ImVec2(0.0f, 0.0f),
                    size, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
            if (spinner)
                DrawSpinner(ImGui::GetForegroundDrawList(), size, now, uiScale);
            return;
        }
        Capture(runtime);
        if (spinner && state == 1 && elapsedMs >= kSettlingSpinnerMs)
            DrawSpinner(ImGui::GetForegroundDrawList(), size, now, uiScale);
    }
}
