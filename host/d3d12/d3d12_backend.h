#pragma once

// host::Backend for a D3D12 swapchain of the game (the interface is
// host_backend.h).
//
// reshade::api objects: ReShade's own D3D12 implementation
// (external/reshade/source/d3d12/d3d12_impl_device/command_queue/
// command_list/command_list_immediate/type_convert.cpp, BSD-3-Clause,
// Copyright (C) 2021 Patrick Mours), compiled into the host unchanged with
// the shims in host/d3d12/shims. One device_impl per ID3D12Device and one
// command_queue_impl per presenting queue, kept for the life of the process
// (never destroyed: their destructors would run under the loader lock).
//
// Per present (d3d12_hooks.cpp calls these around host::OnPresent):
//   BeginPresent  the current back buffer (GetBuffer, held until EndPresent);
//   OnPresent     core overlay (records on the queue's immediate command
//                 list: the export copies the back buffer there), then
//                 ImGuiRender, then core present;
//   EndPresent    the immediate list executed on the presenting queue (its
//                 own fence per allocator), the back buffer released; then
//                 the hook forwards the real Present.
// Holding no back buffer reference between presents keeps the game's
// ResizeBuffers working.
//
// ImGui: imgui_impl_dx12 (ImGui 1.92.2b backend, MIT) with a shader visible
// descriptor heap of our own. The core passes resource_view handles as
// ImTextureID, as under ReShade; in D3D12 those are CPU descriptor handles of
// ReShade's (non shader visible) view heap, so before drawing every such id
// is copied into a per frame slot of our heap and the draw command gets that
// slot's GPU handle. ImGui's own textures (the font atlas) are left as the
// backend made them. Descriptor heap state of ReShade's immediate command list
// is reset by flushing it before and after the ImGui draw (flush clears its
// cached root signature and heaps).

#include "host_backend.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace reshade::d3d12
{
    class device_impl;
    class command_queue_impl;
}

namespace host::d3d12
{
    // The reshade::api device and queue objects for a native device/queue,
    // made once. Null on failure (logged).
    reshade::d3d12::device_impl* DeviceFor(ID3D12Device* device);
    reshade::d3d12::command_queue_impl* QueueFor(reshade::d3d12::device_impl* device, ID3D12CommandQueue* queue);

    class D3D12Backend final : public host::Backend
    {
    public:
        // `swapchain` is not referenced (the game owns it; the hooks drop the
        // backend when the game makes a new swapchain for the window).
        D3D12Backend(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, reshade::d3d12::device_impl* device,
            reshade::d3d12::command_queue_impl* apiQueue, HWND window);
        ~D3D12Backend() override;

        IDXGISwapChain3* SwapChain() const { return m_swapchain; }
        // False: nothing to draw this present (no back buffer); logged once.
        bool BeginPresent();
        void EndPresent();
        // ResizeBuffers is about to run / ran: the GPU is idled before it, the
        // description is read again after it.
        void BeforeResize();
        void AfterResize();

        // host::Backend
        reshade::api::device_api Api() const override { return reshade::api::device_api::d3d12; }
        reshade::api::device* Device() override;
        reshade::api::command_queue* Queue() override;
        reshade::api::resource CurrentBackBuffer() override;
        reshade::api::format BackBufferFormat() const override;
        void BackBufferSize(uint32_t& w, uint32_t& h) const override;
        HWND Window() const override { return m_window; }
        void* Native() const override { return m_swapchain; }
        bool ImGuiInit() override;
        void ImGuiNewFrame() override;
        void ImGuiRender(ImDrawData* data) override;
        void ImGuiShutdown() override;
        ImTextureID TextureId(reshade::api::resource_view view) override;
        uint32_t BackBufferCount() const override { return m_bufferCount; }
        uint32_t BackBufferIndex() const override { return m_bufferIndex; }
        reshade::api::resource BackBuffer(uint32_t index) override;

    private:
        void ReadDescription();
        bool CreateHeaps();
        void TranslateTextures(ImDrawData* data);

        IDXGISwapChain3* m_swapchain = nullptr;
        ID3D12CommandQueue* m_queue = nullptr; // referenced
        ID3D12Device* m_device = nullptr;      // referenced
        reshade::d3d12::device_impl* m_apiDevice = nullptr;
        reshade::d3d12::command_queue_impl* m_apiQueue = nullptr;
        HWND m_window = nullptr;

        DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_bufferCount = 0;
        uint32_t m_bufferIndex = 0;
        ID3D12Resource* m_current = nullptr; // between BeginPresent and EndPresent

        // ImGui
        bool m_imgui = false;
        DXGI_FORMAT m_imguiFormat = DXGI_FORMAT_UNKNOWN;
        ID3D12DescriptorHeap* m_srvHeap = nullptr; // shader visible
        ID3D12DescriptorHeap* m_rtvHeap = nullptr;
        uint32_t m_srvIncrement = 0;
        uint32_t m_rtvIncrement = 0;
        std::vector<bool> m_textureSlotUsed; // slots for ImGui's own textures
        uint64_t m_frame = 0;
        std::unordered_map<uint64_t, uint64_t> m_frameIds; // CPU handle -> GPU handle, this frame
        uint32_t m_frameSlots = 0;
        bool m_loggedSlotsFull = false;
        bool m_loggedNoBackBuffer = false;

    public:
        // imgui_impl_dx12 descriptor callbacks (static, via InitInfo::UserData).
        bool AllocTextureSlot(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu);
        void FreeTextureSlot(D3D12_CPU_DESCRIPTOR_HANDLE cpu);

        static constexpr uint32_t kTextureSlots = 64;     // ImGui's own textures (font atlas pages)
        static constexpr uint32_t kFramesInFlight = 4;    // > DXGI's default frame latency of 3
        static constexpr uint32_t kSlotsPerFrame = 512;   // core textures drawn in one frame
    };
}
