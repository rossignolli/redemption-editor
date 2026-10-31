// host::Backend over a D3D12 swapchain (d3d12_backend.h has the design).

#include "d3d12_backend.h"

#include "host_log.h"

// device first: its descriptor_heap.hpp brings com_ptr.hpp, which the queue headers use.
#include "d3d12/d3d12_impl_device.hpp"
#include "d3d12/d3d12_impl_command_queue.hpp"
#include "d3d12/d3d12_impl_type_convert.hpp"

#include <backends/imgui_impl_dx12.h>

#include <mutex>
#include <new>

namespace host::d3d12
{
    using namespace reshade::api;

    namespace
    {
        std::mutex g_objectsMutex;
        std::unordered_map<ID3D12Device*, reshade::d3d12::device_impl*> g_devices;
        std::unordered_map<ID3D12CommandQueue*, reshade::d3d12::command_queue_impl*> g_queues;

        void AllocSlot(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
        {
            auto* backend = static_cast<D3D12Backend*>(info->UserData);
            if (!backend->AllocTextureSlot(*cpu, *gpu))
            {
                cpu->ptr = 0;
                gpu->ptr = 0;
            }
        }

        void FreeSlot(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
        {
            static_cast<D3D12Backend*>(info->UserData)->FreeTextureSlot(cpu);
        }
    }

    reshade::d3d12::device_impl* DeviceFor(ID3D12Device* device)
    {
        if (!device)
            return nullptr;
        std::lock_guard<std::mutex> lock(g_objectsMutex);
        const auto it = g_devices.find(device);
        if (it != g_devices.end())
            return it->second;
        auto* impl = new (std::nothrow) reshade::d3d12::device_impl(device);
        if (!impl)
        {
            HOST_ERROR("HOST_D3D12 failed reason=device_impl_alloc");
            return nullptr;
        }
        device->AddRef(); // kept with the impl (never destroyed: d3d12_backend.h)
        g_devices[device] = impl;
        HOST_LOG("HOST_D3D12 device_impl device=%p (the \"mipmap generation signature\" error just before is expected: no mipmap shader in the host, generate_mipmaps unused)",
            static_cast<void*>(device));
        return impl;
    }

    reshade::d3d12::command_queue_impl* QueueFor(reshade::d3d12::device_impl* device, ID3D12CommandQueue* queue)
    {
        if (!device || !queue)
            return nullptr;
        std::lock_guard<std::mutex> lock(g_objectsMutex);
        const auto it = g_queues.find(queue);
        if (it != g_queues.end())
            return it->second;
        const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
        if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            // ReShade's D3D12 impl only makes an immediate command list on a
            // direct queue; a swapchain is always made on one.
            HOST_ERROR("HOST_D3D12 failed reason=present_queue_not_direct type=%d", static_cast<int>(desc.Type));
            return nullptr;
        }
        auto* impl = new (std::nothrow) reshade::d3d12::command_queue_impl(device, queue);
        if (!impl)
        {
            HOST_ERROR("HOST_D3D12 failed reason=queue_impl_alloc");
            return nullptr;
        }
        if (!impl->get_immediate_command_list())
        {
            HOST_ERROR("HOST_D3D12 failed reason=immediate_command_list queue=%p", static_cast<void*>(queue));
            delete impl;
            return nullptr;
        }
        queue->AddRef();
        g_queues[queue] = impl;
        HOST_LOG("HOST_D3D12 queue_impl queue=%p type=direct priority=%d node_mask=%u timestamp_hz=%llu", static_cast<void*>(queue),
            static_cast<int>(desc.Priority), desc.NodeMask, static_cast<unsigned long long>(impl->get_timestamp_frequency()));
        return impl;
    }

    // ------------------------------------------------------------------
    // D3D12Backend
    // ------------------------------------------------------------------

    D3D12Backend::D3D12Backend(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, reshade::d3d12::device_impl* device,
        reshade::d3d12::command_queue_impl* apiQueue, HWND window) :
        m_swapchain(swapchain), m_queue(queue), m_apiDevice(device), m_apiQueue(apiQueue), m_window(window)
    {
        m_queue->AddRef();
        m_device = reinterpret_cast<ID3D12Device*>(m_apiDevice->get_native());
        m_device->AddRef();
        ReadDescription();
    }

    D3D12Backend::~D3D12Backend()
    {
        // ImGuiShutdown ran before (host::OnBackendDestroyed); only our own
        // objects here.
        if (m_current)
            m_current->Release();
        if (m_srvHeap)
            m_srvHeap->Release();
        if (m_rtvHeap)
            m_rtvHeap->Release();
        m_device->Release();
        m_queue->Release();
    }

    void D3D12Backend::ReadDescription()
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (FAILED(m_swapchain->GetDesc1(&desc)))
            return;
        m_format = desc.Format;
        m_width = desc.Width;
        m_height = desc.Height;
        m_bufferCount = desc.BufferCount;
        if (!m_window)
            m_swapchain->GetHwnd(&m_window);
        HOST_LOG("HOST_D3D12 swapchain=%p window=%p size=%ux%u format=%u buffers=%u swap_effect=%d flags=0x%X", static_cast<void*>(m_swapchain),
            static_cast<void*>(m_window), m_width, m_height, static_cast<unsigned>(m_format), m_bufferCount, static_cast<int>(desc.SwapEffect),
            desc.Flags);
    }

    device* D3D12Backend::Device() { return m_apiDevice; }
    command_queue* D3D12Backend::Queue() { return m_apiQueue; }
    resource D3D12Backend::CurrentBackBuffer() { return resource{reinterpret_cast<uint64_t>(m_current)}; }
    format D3D12Backend::BackBufferFormat() const { return reshade::d3d12::convert_format(m_format); }

    void D3D12Backend::BackBufferSize(uint32_t& w, uint32_t& h) const
    {
        w = m_width;
        h = m_height;
    }

    resource D3D12Backend::BackBuffer(uint32_t index)
    {
        if (m_current && index == m_bufferIndex)
            return CurrentBackBuffer();
        // The swapchain keeps its buffers alive; no reference of ours stays.
        ID3D12Resource* buffer = nullptr;
        if (FAILED(m_swapchain->GetBuffer(index, IID_PPV_ARGS(&buffer))) || !buffer)
            return resource{0};
        buffer->Release();
        return resource{reinterpret_cast<uint64_t>(buffer)};
    }

    bool D3D12Backend::BeginPresent()
    {
        if (m_current)
        {
            m_current->Release();
            m_current = nullptr;
        }
        m_bufferIndex = m_swapchain->GetCurrentBackBufferIndex();
        if (FAILED(m_swapchain->GetBuffer(m_bufferIndex, IID_PPV_ARGS(&m_current))) || !m_current)
        {
            m_current = nullptr;
            if (!m_loggedNoBackBuffer)
            {
                m_loggedNoBackBuffer = true;
                HOST_ERROR("HOST_D3D12 failed reason=get_back_buffer index=%u", m_bufferIndex);
            }
            return false;
        }
        return true;
    }

    void D3D12Backend::EndPresent()
    {
        // Everything the core and ImGui recorded, on the presenting queue,
        // before the game's Present (a D3D12 queue runs in submission order).
        m_apiQueue->flush_immediate_command_list();
        if (m_current)
        {
            m_current->Release();
            m_current = nullptr;
        }
    }

    void D3D12Backend::BeforeResize()
    {
        // Nothing of ours may still use the old buffers (we hold no
        // reference; the GPU may still run our last submission).
        m_apiQueue->wait_idle();
        if (m_current)
        {
            m_current->Release();
            m_current = nullptr;
        }
    }

    void D3D12Backend::AfterResize() { ReadDescription(); }

    // ------------------------------------------------------------------
    // ImGui
    // ------------------------------------------------------------------

    bool D3D12Backend::CreateHeaps()
    {
        if (m_srvHeap && m_rtvHeap)
            return true;
        D3D12_DESCRIPTOR_HEAP_DESC srv{};
        srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv.NumDescriptors = kTextureSlots + kFramesInFlight * kSlotsPerFrame;
        srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        D3D12_DESCRIPTOR_HEAP_DESC rtv{};
        rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv.NumDescriptors = 1; // RTVs are read when recorded: one slot, rewritten per present
        if (FAILED(m_device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&m_srvHeap))) || FAILED(m_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&m_rtvHeap))))
        {
            HOST_ERROR("HOST_D3D12 failed reason=imgui_descriptor_heaps");
            return false;
        }
        m_srvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_rtvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        m_textureSlotUsed.assign(kTextureSlots, false);
        return true;
    }

    bool D3D12Backend::AllocTextureSlot(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu)
    {
        for (uint32_t i = 0; i < kTextureSlots; ++i)
        {
            if (m_textureSlotUsed[i])
                continue;
            m_textureSlotUsed[i] = true;
            cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += static_cast<SIZE_T>(i) * m_srvIncrement;
            gpu = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<UINT64>(i) * m_srvIncrement;
            return true;
        }
        HOST_ERROR("HOST_D3D12 failed reason=imgui_texture_slots_full slots=%u", kTextureSlots);
        return false;
    }

    void D3D12Backend::FreeTextureSlot(D3D12_CPU_DESCRIPTOR_HANDLE cpu)
    {
        if (!m_srvHeap || !cpu.ptr)
            return;
        const SIZE_T start = m_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
        if (cpu.ptr < start)
            return;
        const SIZE_T index = (cpu.ptr - start) / m_srvIncrement;
        if (index < kTextureSlots)
            m_textureSlotUsed[index] = false;
    }

    bool D3D12Backend::ImGuiInit()
    {
        if (m_imgui)
            return true;
        if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererUserData)
        {
            HOST_ERROR("HOST_IMGUI failed reason=renderer_already_set (another swapchain's renderer still up)");
            return false;
        }
        // imgui_impl_dx12 compiles its two shaders with D3DCompile
        // (d3dcompiler_47.dll, delay loaded by the host): the system copy,
        // loaded first so the delay load never fails with an exception.
        if (!LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        {
            HOST_ERROR("HOST_IMGUI failed reason=d3dcompiler_47_missing error=%lu", GetLastError());
            return false;
        }
        if (!CreateHeaps())
            return false;
        ImGui_ImplDX12_InitInfo info;
        info.Device = m_device;
        info.CommandQueue = m_queue;
        info.NumFramesInFlight = static_cast<int>(kFramesInFlight);
        info.RTVFormat = m_format;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.UserData = this;
        info.SrvDescriptorHeap = m_srvHeap;
        info.SrvDescriptorAllocFn = &AllocSlot;
        info.SrvDescriptorFreeFn = &FreeSlot;
        if (!ImGui_ImplDX12_Init(&info))
        {
            HOST_ERROR("HOST_IMGUI failed reason=imgui_impl_dx12_init");
            return false;
        }
        if (!ImGui_ImplDX12_CreateDeviceObjects())
        {
            HOST_ERROR("HOST_IMGUI failed reason=imgui_impl_dx12_device_objects (root signature or D3DCompile)");
            ImGui_ImplDX12_Shutdown();
            return false;
        }
        m_imgui = true;
        m_imguiFormat = m_format;
        HOST_LOG("HOST_IMGUI renderer=imgui_impl_dx12 rtv_format=%u frames_in_flight=%u srv_heap=%u", static_cast<unsigned>(m_format), kFramesInFlight,
            kTextureSlots + kFramesInFlight * kSlotsPerFrame);
        return true;
    }

    void D3D12Backend::ImGuiNewFrame()
    {
        if (m_imgui && m_imguiFormat != m_format)
        {
            // ResizeBuffers changed the format: the pipeline follows it.
            HOST_LOG("HOST_IMGUI rebuild reason=format old=%u new=%u", static_cast<unsigned>(m_imguiFormat), static_cast<unsigned>(m_format));
            ImGuiShutdown();
            if (!ImGuiInit())
                return;
        }
        if (m_imgui)
            ImGui_ImplDX12_NewFrame();
    }

    void D3D12Backend::TranslateTextures(ImDrawData* data)
    {
        ++m_frame;
        m_frameIds.clear();
        m_frameSlots = 0;
        const uint32_t base = kTextureSlots + static_cast<uint32_t>(m_frame % kFramesInFlight) * kSlotsPerFrame;
        const D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        for (int l = 0; l < data->CmdListsCount; ++l)
        {
            ImDrawList* list = data->CmdLists[l];
            for (ImDrawCmd& cmd : list->CmdBuffer)
            {
                if (cmd.UserCallback || cmd.TexRef._TexData || cmd.TexRef._TexID == ImTextureID_Invalid)
                    continue; // ImGui's own texture, or no texture
                const uint64_t id = static_cast<uint64_t>(cmd.TexRef._TexID);
                const auto it = m_frameIds.find(id);
                if (it != m_frameIds.end())
                {
                    cmd.TexRef._TexID = static_cast<ImTextureID>(it->second);
                    continue;
                }
                if (m_frameSlots >= kSlotsPerFrame)
                {
                    if (!m_loggedSlotsFull)
                    {
                        m_loggedSlotsFull = true;
                        HOST_WARN("HOST_IMGUI textures_per_frame_full slots=%u (the rest not drawn)", kSlotsPerFrame);
                    }
                    cmd.ElemCount = 0;
                    continue;
                }
                const uint32_t slot = base + m_frameSlots++;
                D3D12_CPU_DESCRIPTOR_HANDLE dst = cpuStart;
                dst.ptr += static_cast<SIZE_T>(slot) * m_srvIncrement;
                m_device->CopyDescriptorsSimple(1, dst, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(id)}, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                const uint64_t gpu = gpuStart.ptr + static_cast<uint64_t>(slot) * m_srvIncrement;
                m_frameIds.emplace(id, gpu);
                cmd.TexRef._TexID = static_cast<ImTextureID>(gpu);
            }
        }
    }

    void D3D12Backend::ImGuiRender(ImDrawData* data)
    {
        if (!m_imgui || !m_current || !data)
            return;
        command_list* cmd = m_apiQueue->get_immediate_command_list();
        // The overlay's work (the export's copy of the back buffer without
        // the UI) goes first, and ReShade's list forgets its bound heaps.
        m_apiQueue->flush_immediate_command_list();
        if (data->CmdListsCount > 0)
            TranslateTextures(data);
        const resource back{reinterpret_cast<uint64_t>(m_current)};
        cmd->barrier(back, resource_usage::present, resource_usage::render_target);
        auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd->get_native());
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        m_device->CreateRenderTargetView(m_current, nullptr, rtv);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {m_srvHeap};
        list->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(data, list);
        cmd->barrier(back, resource_usage::render_target, resource_usage::present);
        // Submitted now: the core's present callback records on a clean list.
        m_apiQueue->flush_immediate_command_list();
    }

    void D3D12Backend::ImGuiShutdown()
    {
        if (!m_imgui)
            return;
        m_apiQueue->wait_idle(); // ImGui's buffers may still be in use
        ImGui_ImplDX12_Shutdown();
        m_imgui = false;
    }

    ImTextureID D3D12Backend::TextureId(resource_view view)
    {
        // The CPU descriptor handle, as the core passes it; TranslateTextures
        // maps it to a shader visible slot when drawn.
        return static_cast<ImTextureID>(view.handle);
    }
}

// ReShade's d3d12_impl_device.cpp asks this (dll_main.cpp in ReShade): the
// host runs on Windows 10 or newer only.
bool is_windows7()
{
    return false;
}
