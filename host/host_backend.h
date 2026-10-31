#pragma once

// The graphics API side of the Redemption Editor host.
// One Backend per swapchain the game presents; host_runtime.cpp drives the
// per present order: core overlay (the back buffer still without the UI, the
// export copies here), ImGui drawn into the back buffer, core present, then
// the backend forwards the real present.

#include <imgui.h>
#include <reshade.hpp>

#include <Windows.h>

#include <cstdint>

namespace host
{
    // One per swapchain. Called on the thread that presents (render thread).
    class Backend
    {
    public:
        virtual ~Backend() = default;
        virtual reshade::api::device_api Api() const = 0;
        virtual reshade::api::device* Device() = 0;
        virtual reshade::api::command_queue* Queue() = 0;          // the queue that presents
        virtual reshade::api::resource CurrentBackBuffer() = 0;     // image about to be presented
        virtual reshade::api::format BackBufferFormat() const = 0;
        virtual void BackBufferSize(uint32_t& w, uint32_t& h) const = 0;
        virtual HWND Window() const = 0;
        virtual void* Native() const = 0;                           // VkSwapchainKHR / IDXGISwapChain*
        // ImGui renderer for this API: init once, draw the frame's ImDrawData into the back buffer
        // just before the real present (after the core's overlay callback).
        virtual bool ImGuiInit() = 0;
        virtual void ImGuiNewFrame() = 0;
        virtual void ImGuiRender(ImDrawData* data) = 0;
        virtual void ImGuiShutdown() = 0;
        // Make texture views created through Device() usable as ImTextureID (the core passes
        // resource_view.handle as ImTextureID, as under ReShade).
        virtual ImTextureID TextureId(reshade::api::resource_view view) = 0;

        // Changes by host-vk (appended to the contract): the swapchain's images
        // by index, for effect_runtime::get_back_buffer / get_back_buffer_count /
        // get_current_back_buffer_index (the core reaches the back buffer through
        // get_current_back_buffer, which ReShade's header builds from these).
        virtual uint32_t BackBufferCount() const = 0;
        virtual uint32_t BackBufferIndex() const = 0;
        virtual reshade::api::resource BackBuffer(uint32_t index) = 0;
    };

    // Called by the backend from inside the present hook, before forwarding the present.
    // HostRuntime does: input begin frame, ImGui new frame, core overlay, ImGui render,
    // core present callback.
    void OnPresent(Backend& backend);
    void OnBackendDestroyed(Backend& backend);

    // Changes by host-vk: false once the host stood down or failed (the
    // ReShade add-on owns the editor, or an init error): the backend then
    // forwards presents untouched.
    bool Active();
}
