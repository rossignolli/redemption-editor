// Render export: the memory readings of export_memory.h. DXGI is loaded
// from the process (dxgi.dll is there for D3D12 and for Vulkan's swap chain
// interop); no link dependency, nothing of the game is called.

#include "export_memory.h"

#include <Windows.h>
#include <Psapi.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <string>

namespace render_export
{
    namespace
    {
        std::string Narrow(const wchar_t* text)
        {
            char out[256]{};
            WideCharToMultiByte(CP_UTF8, 0, text, -1, out, static_cast<int>(sizeof(out)) - 1, nullptr, nullptr);
            for (char* c = out; *c; ++c)
                if (*c == ',')
                    *c = ' '; // the log's field separator
            return out;
        }

        std::uint64_t LocalUsage(IDXGIAdapter3* adapter)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            return SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) ? info.CurrentUsage : 0;
        }
    }

    std::string MemoryProbe::Open(std::uint64_t nativeDevice, bool d3d12)
    {
        Close();
        using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        if (!dxgi)
            dxgi = LoadLibraryW(L"dxgi.dll");
        const auto create = dxgi ? reinterpret_cast<CreateFactory>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
        IDXGIFactory4* factory = nullptr;
        if (!create || FAILED(create(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) || !factory)
            return "RENDER_EXPORT,phase=mem_adapter,match=none,reason=no_dxgi_factory4";
        IDXGIAdapter3* chosen = nullptr;
        const char* match = "none";
        if (d3d12 && nativeDevice)
        {
            // The device's own adapter.
            const LUID luid = reinterpret_cast<ID3D12Device*>(nativeDevice)->GetAdapterLuid();
            if (SUCCEEDED(factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&chosen))) && chosen)
                match = "luid";
        }
        if (!chosen)
        {
            // Vulkan: the adapter holding the most of this process's dedicated
            // memory is the one the game renders on (an iGPU next to it holds
            // little); with no usage anywhere, the biggest adapter.
            std::uint64_t bestUsage = 0, bestDedicated = 0;
            IDXGIAdapter3* byUsage = nullptr;
            IDXGIAdapter3* bySize = nullptr;
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
            {
                DXGI_ADAPTER_DESC1 desc{};
                IDXGIAdapter3* a3 = nullptr;
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3))) && a3)
                {
                    const std::uint64_t usage = LocalUsage(a3);
                    if (usage > bestUsage)
                    {
                        if (byUsage)
                            byUsage->Release();
                        byUsage = a3;
                        a3->AddRef();
                        bestUsage = usage;
                    }
                    if (desc.DedicatedVideoMemory > bestDedicated)
                    {
                        if (bySize)
                            bySize->Release();
                        bySize = a3;
                        a3->AddRef();
                        bestDedicated = desc.DedicatedVideoMemory;
                    }
                    a3->Release();
                }
                adapter->Release();
            }
            if (byUsage)
            {
                chosen = byUsage;
                match = "usage";
                if (bySize)
                    bySize->Release();
            }
            else if (bySize)
            {
                chosen = bySize;
                match = "largest";
            }
        }
        factory->Release();
        if (!chosen)
            return "RENDER_EXPORT,phase=mem_adapter,match=none,reason=no_adapter";
        m_adapter = chosen;
        DXGI_ADAPTER_DESC1 desc{};
        chosen->GetDesc1(&desc);
        return std::string("RENDER_EXPORT,phase=mem_adapter,match=") + match + ",name=" + Narrow(desc.Description) +
            ",dedicated_mb=" + std::to_string(MemMb(desc.DedicatedVideoMemory)) + ",shared_mb=" + std::to_string(MemMb(desc.SharedSystemMemory));
    }

    void MemoryProbe::Close()
    {
        if (m_adapter)
            static_cast<IDXGIAdapter3*>(m_adapter)->Release();
        m_adapter = nullptr;
    }

    MemorySample MemoryProbe::Sample() const
    {
        MemorySample s;
        if (auto* adapter = static_cast<IDXGIAdapter3*>(m_adapter))
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
            s.vram = SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
                SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared));
            if (s.vram)
            {
                s.vramUsed = local.CurrentUsage;
                s.vramBudget = local.Budget;
                s.sharedUsed = shared.CurrentUsage;
                s.sharedBudget = shared.Budget;
            }
        }
        PROCESS_MEMORY_COUNTERS_EX process{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process)))
            s.privateBytes = process.PrivateUsage;
        PERFORMANCE_INFORMATION performance{};
        if (GetPerformanceInfo(&performance, sizeof(performance)))
        {
            s.commit = static_cast<std::uint64_t>(performance.CommitTotal) * performance.PageSize;
            s.commitLimit = static_cast<std::uint64_t>(performance.CommitLimit) * performance.PageSize;
        }
        return s;
    }
}
