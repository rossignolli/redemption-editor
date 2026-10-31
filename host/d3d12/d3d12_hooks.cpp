// D3D12/DXGI hooks of the host (d3d12_hooks.h has the order).

#include "d3d12_hooks.h"

#include "d3d12_backend.h"
#include "host_log.h"
#include "host_owner.h"

#include <MinHook.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h> // K32EnumProcessModules (kernel32)

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace host::d3d12
{
    // Detours (InstallSwapChainHooks / InstallFactoryHooks take their addresses).
    HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* swapchain, UINT sync, UINT flags);
    HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain1* swapchain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters);
    HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* swapchain, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags);
    HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain3* swapchain, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags,
        const UINT* nodeMasks, IUnknown* const* queues);

    namespace
    {
        // vtable slots (dxgi.h order).
        constexpr int kFactoryCreateSwapChain = 10;        // IDXGIFactory
        constexpr int kFactoryCreateSwapChainForHwnd = 15; // IDXGIFactory2
        constexpr int kSwapChainPresent = 8;               // IDXGISwapChain
        constexpr int kSwapChainResizeBuffers = 13;        // IDXGISwapChain
        constexpr int kSwapChainPresent1 = 22;             // IDXGISwapChain1
        constexpr int kSwapChainResizeBuffers1 = 39;       // IDXGISwapChain3

        using PFN_CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
        using PFN_CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
        using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
        using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
        using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
        using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        using PFN_ResizeBuffers1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);

        PFN_CreateDevice g_createDevice = nullptr;
        PFN_CreateSwapChain g_createSwapChain = nullptr;
        PFN_CreateSwapChainForHwnd g_createSwapChainForHwnd = nullptr;
        PFN_Present g_present = nullptr;
        PFN_Present1 g_present1 = nullptr;
        PFN_ResizeBuffers g_resizeBuffers = nullptr;
        PFN_ResizeBuffers1 g_resizeBuffers1 = nullptr;

        std::mutex g_mutex; // hooks and the backend lists
        std::atomic<bool> g_watcherStarted{false};
        std::atomic<bool> g_deviceHooked{false};
        std::atomic<bool> g_factoryHooked{false};
        std::atomic<bool> g_swapchainHooked{false};
        std::atomic<bool> g_disabled{false};    // an exception in the present work
        std::atomic<bool> g_standingDown{false}; // the add-on owns the editor
        std::atomic<bool> g_presented{false};   // the host ran a present (owner from then on)

        struct Entry
        {
            IDXGISwapChain* swapchain; // as the creation call returned it
            IUnknown* identity;        // its IUnknown (not referenced), for a lookup by another interface pointer
            D3D12Backend* backend;
        };
        std::vector<Entry> g_backends;
        std::vector<D3D12Backend*> g_retired; // replaced swapchains, destroyed on the next present

        thread_local int t_createDepth = 0;
        thread_local int t_presentDepth = 0;

        std::wstring SystemPath(const wchar_t* name)
        {
            wchar_t dir[MAX_PATH]{};
            const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
            if (!n || n >= MAX_PATH)
                return name;
            return std::wstring(dir) + L"\\" + name;
        }

        // REDEMPTION_EDITOR_HOST_MODE, set by RedemptionEditor.asi from
        // [RedemptionEditor] Host (src/host_layer_env.h): "auto" or "native";
        // unset with Host=reshade or no host installed.
        bool HostModeIs(const wchar_t* value)
        {
            wchar_t mode[32]{};
            GetEnvironmentVariableW(L"REDEMPTION_EDITOR_HOST_MODE", mode, 32);
            return _wcsicmp(mode, value) == 0;
        }

        // The add-on took the owner mutex and the host has not presented:
        // the editor runs through ReShade in this process. With Host=native
        // the ASI holds the mutex for the host (the add-on stands down).
        bool OwnedByAddon()
        {
            if (g_presented.load() || HostModeIs(L"native"))
                return false;
            wchar_t name[64];
            OwnerMutexName(name);
            HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name);
            if (!mutex)
                return false;
            CloseHandle(mutex);
            return true;
        }

        // A ReShade in this process (any module exporting ReShadeRegisterAddon:
        // ReShade64.dll as the global Vulkan layer, which in DX12 mode is still
        // loaded for the game's enumeration instance and hooks d3d12.dll and
        // dxgi.dll itself, or a dxgi.dll/d3d12.dll proxy). The host's D3D12
        // path does not stack its inline hooks with ReShade's.
        HMODULE ReShadeModule()
        {
            HMODULE modules[1024];
            DWORD bytes = 0;
            if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
                return nullptr;
            const DWORD count = (bytes > sizeof(modules) ? sizeof(modules) : bytes) / sizeof(HMODULE);
            for (DWORD i = 0; i < count; ++i)
                if (GetProcAddress(modules[i], "ReShadeRegisterAddon"))
                    return modules[i];
            return nullptr;
        }

        // True from the first time the add-on owns the editor or a ReShade is
        // in the process: no hook is installed past that point, hooks already
        // in place pass every call straight through, no backend is made.
        bool StandDownIfOwned(const char* where)
        {
            if (g_standingDown.load())
                return true;
            if (g_presented.load())
                return false; // the host runs the editor; host_runtime decides from here
            if (OwnedByAddon())
            {
                g_standingDown = true;
                HOST_WARN("HOST_D3D12 standing_down reason=editor_owned_by_reshade_addon at=%s (no D3D12 hooks past this point)", where);
                return true;
            }
            if (const HMODULE reshade = ReShadeModule())
            {
                g_standingDown = true;
                wchar_t path[MAX_PATH]{};
                GetModuleFileNameW(reshade, path, MAX_PATH);
                HOST_WARN("HOST_D3D12 standing_down reason=reshade_in_process module=%ls at=%s (no D3D12 hooks past this point; hooks already in place pass through)",
                    path, where);
                return true;
            }
            return false;
        }

        // The first calls of every detour, logged (host_log writes each line
        // with an unbuffered WriteFile: a line is in the file even if the
        // process dies right after).
        void LogEnter(std::atomic<unsigned>& count, const char* name)
        {
            const unsigned n = ++count;
            if (n <= 3)
                HOST_LOG("HOST_D3D12 enter fn=%s n=%u standing_down=%d disabled=%d", name, n, g_standingDown.load() ? 1 : 0, g_disabled.load() ? 1 : 0);
        }
        std::atomic<unsigned> g_enterCreateDevice{0}, g_enterCreateSwapChain{0}, g_enterCreateSwapChainForHwnd{0}, g_enterPresent{0}, g_enterPresent1{0},
            g_enterResize{0}, g_enterResize1{0};

        bool MinHookReady()
        {
            static const MH_STATUS status = MH_Initialize();
            if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
            {
                HOST_ERROR("HOST_D3D12 failed reason=minhook_init status=%s", MH_StatusToString(status));
                return false;
            }
            return true;
        }

        template <typename T>
        bool Hook(void* target, void* detour, T& original, const char* name)
        {
            if (!target)
            {
                HOST_ERROR("HOST_D3D12 failed reason=hook_target_missing fn=%s", name);
                return false;
            }
            // The trampoline lands in `original` before the hook is enabled: a
            // call racing the enable never sees a null original.
            MH_STATUS status = MH_CreateHook(target, detour, reinterpret_cast<void**>(&original));
            if (status == MH_OK)
                status = MH_EnableHook(target);
            if (status != MH_OK)
            {
                HOST_ERROR("HOST_D3D12 failed reason=hook fn=%s target=%p status=%s", name, target, MH_StatusToString(status));
                if (original)
                    MH_RemoveHook(target);
                original = nullptr;
                return false;
            }
            HOST_LOG("HOST_D3D12 hook fn=%s target=%p", name, target);
            return true;
        }

        void* VtableEntry(void* object, int slot) { return (*static_cast<void***>(object))[slot]; }

        // ------------------------------------------------------------------
        // Backends
        // ------------------------------------------------------------------

        D3D12Backend* FindLocked(IDXGISwapChain* swapchain)
        {
            for (const Entry& e : g_backends)
                if (e.swapchain == swapchain)
                    return e.backend;
            // Called through another interface pointer of the same object.
            IUnknown* identity = nullptr;
            if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&identity))) || !identity)
                return nullptr;
            identity->Release();
            for (const Entry& e : g_backends)
                if (e.identity == identity)
                    return e.backend;
            return nullptr;
        }

        D3D12Backend* Find(IDXGISwapChain* swapchain)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            return FindLocked(swapchain);
        }

        void InstallSwapChainHooks(IDXGISwapChain3* swapchain)
        {
            if (StandDownIfOwned("swapchain_hooks") || g_swapchainHooked.exchange(true))
                return;
            bool ok = Hook(VtableEntry(swapchain, kSwapChainPresent), reinterpret_cast<void*>(&DetourPresent), g_present, "IDXGISwapChain::Present");
            ok = Hook(VtableEntry(swapchain, kSwapChainPresent1), reinterpret_cast<void*>(&DetourPresent1), g_present1, "IDXGISwapChain1::Present1") && ok;
            ok = Hook(VtableEntry(swapchain, kSwapChainResizeBuffers), reinterpret_cast<void*>(&DetourResizeBuffers), g_resizeBuffers,
                     "IDXGISwapChain::ResizeBuffers") && ok;
            ok = Hook(VtableEntry(swapchain, kSwapChainResizeBuffers1), reinterpret_cast<void*>(&DetourResizeBuffers1), g_resizeBuffers1,
                     "IDXGISwapChain3::ResizeBuffers1") && ok;
            if (!ok)
                HOST_ERROR("HOST_D3D12 failed reason=swapchain_hooks (the editor may not show)");
        }

        void OnSwapChainCreated(IUnknown* deviceArg, IDXGISwapChain* swapchain, HWND window, const char* via)
        {
            if (g_disabled.load() || !deviceArg || !swapchain || StandDownIfOwned("create_swapchain"))
                return;
            ID3D12CommandQueue* queue = nullptr;
            if (FAILED(deviceArg->QueryInterface(IID_PPV_ARGS(&queue))) || !queue)
            {
                HOST_LOG("HOST_D3D12 swapchain_skipped via=%s reason=not_a_d3d12_queue", via);
                return;
            }
            IDXGISwapChain3* swapchain3 = nullptr;
            ID3D12Device* device = nullptr;
            if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) || !swapchain3)
            {
                HOST_ERROR("HOST_D3D12 failed reason=no_IDXGISwapChain3 via=%s", via);
                queue->Release();
                return;
            }
            swapchain3->Release(); // the game's object; not referenced by the backend
            if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device)
            {
                HOST_ERROR("HOST_D3D12 failed reason=queue_get_device via=%s", via);
                queue->Release();
                return;
            }
            reshade::d3d12::device_impl* apiDevice = DeviceFor(device);
            reshade::d3d12::command_queue_impl* apiQueue = QueueFor(apiDevice, queue);
            if (!apiDevice || !apiQueue)
            {
                device->Release();
                queue->Release();
                return;
            }
            IUnknown* identity = nullptr;
            swapchain->QueryInterface(IID_PPV_ARGS(&identity));
            if (identity)
                identity->Release();
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                // A new swapchain replaces the window's old one (one flip model
                // swapchain per window): the old backend goes on the next present.
                for (auto it = g_backends.begin(); it != g_backends.end();)
                {
                    if (it->backend->Window() == window || it->swapchain == swapchain || (identity && it->identity == identity))
                    {
                        g_retired.push_back(it->backend);
                        it = g_backends.erase(it);
                    }
                    else
                        ++it;
                }
                auto* backend = new (std::nothrow) D3D12Backend(swapchain3, queue, apiDevice, apiQueue, window);
                if (backend)
                    g_backends.push_back({swapchain, identity, backend});
                HOST_LOG("HOST_START api=d3d12 via=%s swapchain=%p queue=%p device=%p window=%p backends=%zu", via, static_cast<void*>(swapchain),
                    static_cast<void*>(queue), static_cast<void*>(device), static_cast<void*>(window), g_backends.size());
            }
            device->Release();
            queue->Release();
            InstallSwapChainHooks(swapchain3);
        }

        // ------------------------------------------------------------------
        // Present
        // ------------------------------------------------------------------

        void PresentWork(IDXGISwapChain* swapchain)
        {
            std::vector<D3D12Backend*> retired;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                retired.swap(g_retired);
            }
            for (D3D12Backend* old : retired)
            {
                HOST_LOG("HOST_D3D12 backend_destroyed swapchain=%p", static_cast<void*>(old->SwapChain()));
                host::OnBackendDestroyed(*old);
                delete old;
            }
            D3D12Backend* backend = Find(swapchain);
            if (!backend || !backend->BeginPresent())
                return;
            if (!g_presented.exchange(true))
                HOST_LOG("HOST_D3D12 present_first swapchain=%p", static_cast<void*>(swapchain));
            host::OnPresent(*backend);
            backend->EndPresent();
        }

        // No C++ objects in here (SEH): an exception in the host's work turns
        // the host off; the game's present goes on.
        void PresentGuarded(IDXGISwapChain* swapchain)
        {
            __try
            {
                PresentWork(swapchain);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_disabled = true;
                HOST_ERROR("HOST_D3D12 failed reason=exception_in_present code=0x%08lX (host off for this session)", GetExceptionCode());
            }
        }

        void BeforePresent(IDXGISwapChain* swapchain, UINT flags)
        {
            if (g_disabled.load() || g_standingDown.load() || (flags & DXGI_PRESENT_TEST) != 0 || t_presentDepth > 0)
                return;
            if (g_presented.load() && !host::Active())
                return; // the host stood down or failed at its start: presents untouched
            ++t_presentDepth;
            PresentGuarded(swapchain);
            --t_presentDepth;
        }
    }

    namespace
    {
        // SEH around the host's work outside Present (no C++ objects in the
        // __try functions): an exception turns the host off, the game's call
        // goes on.
        void ResizeWork(D3D12Backend* backend, bool before)
        {
            if (before)
                backend->BeforeResize();
            else
                backend->AfterResize();
        }

        bool GuardedResize(D3D12Backend* backend, bool before)
        {
            __try
            {
                ResizeWork(backend, before);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_disabled = true;
                HOST_ERROR("HOST_D3D12 failed reason=exception_in_resize code=0x%08lX (host off for this session)", GetExceptionCode());
                return false;
            }
        }

        D3D12Backend* GuardedFind(IDXGISwapChain* swapchain)
        {
            __try
            {
                return Find(swapchain);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_disabled = true;
                HOST_ERROR("HOST_D3D12 failed reason=exception_in_find code=0x%08lX (host off for this session)", GetExceptionCode());
                return nullptr;
            }
        }

        void GuardedSwapChainCreated(IUnknown* deviceArg, IDXGISwapChain* swapchain, HWND window, const char* via)
        {
            __try
            {
                OnSwapChainCreated(deviceArg, swapchain, window, via);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_disabled = true;
                HOST_ERROR("HOST_D3D12 failed reason=exception_in_swapchain_created code=0x%08lX (host off for this session)", GetExceptionCode());
            }
        }
    }

    HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* swapchain, UINT sync, UINT flags)
    {
        LogEnter(g_enterPresent, "Present");
        // DXGI may route Present through Present1 internally: the depth keeps
        // the host's work to the outer call.
        BeforePresent(swapchain, flags);
        ++t_presentDepth;
        const HRESULT hr = g_present(swapchain, sync, flags);
        --t_presentDepth;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain1* swapchain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
    {
        LogEnter(g_enterPresent1, "Present1");
        BeforePresent(swapchain, flags);
        ++t_presentDepth;
        const HRESULT hr = g_present1(swapchain, sync, flags, parameters);
        --t_presentDepth;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* swapchain, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags)
    {
        LogEnter(g_enterResize, "DetourResizeBuffers");
        D3D12Backend* backend = (g_disabled.load() || g_standingDown.load()) ? nullptr : GuardedFind(swapchain);
        if (backend && !GuardedResize(backend, true))
            backend = nullptr;
        const HRESULT hr = g_resizeBuffers(swapchain, count, w, h, format, flags);
        if (backend)
        {
            if (SUCCEEDED(hr))
                GuardedResize(backend, false);
            else
                HOST_WARN("HOST_D3D12 resize_failed hr=0x%08lX", static_cast<unsigned long>(hr));
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain3* swapchain, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags,
        const UINT* nodeMasks, IUnknown* const* queues)
    {
        LogEnter(g_enterResize1, "DetourResizeBuffers1");
        D3D12Backend* backend = (g_disabled.load() || g_standingDown.load()) ? nullptr : GuardedFind(swapchain);
        if (backend && !GuardedResize(backend, true))
            backend = nullptr;
        const HRESULT hr = g_resizeBuffers1(swapchain, count, w, h, format, flags, nodeMasks, queues);
        if (backend)
        {
            if (SUCCEEDED(hr))
                GuardedResize(backend, false);
            else
                HOST_WARN("HOST_D3D12 resize1_failed hr=0x%08lX", static_cast<unsigned long>(hr));
        }
        return hr;
    }

    // ------------------------------------------------------------------
    // Factory and device
    // ------------------------------------------------------------------

    HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* factory, IUnknown* deviceArg, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
    {
        LogEnter(g_enterCreateSwapChain, "CreateSwapChain");
        ++t_createDepth;
        const HRESULT hr = g_createSwapChain(factory, deviceArg, desc, out);
        --t_createDepth;
        if (t_createDepth == 0 && SUCCEEDED(hr) && out && *out)
            GuardedSwapChainCreated(deviceArg, *out, desc ? desc->OutputWindow : nullptr, "CreateSwapChain");
        return hr;
    }

    HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* deviceArg, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** out)
    {
        LogEnter(g_enterCreateSwapChainForHwnd, "CreateSwapChainForHwnd");
        ++t_createDepth;
        const HRESULT hr = g_createSwapChainForHwnd(factory, deviceArg, window, desc, fullscreen, output, out);
        --t_createDepth;
        if (t_createDepth == 0 && SUCCEEDED(hr) && out && *out)
            GuardedSwapChainCreated(deviceArg, *out, window, "CreateSwapChainForHwnd");
        return hr;
    }

    namespace
    {
        // Through a dummy factory of the system dxgi.dll: its code is what
        // every factory of the process runs (ReShade's dxgi.dll wrapper ends
        // there too).
        void InstallFactoryHooks(const char* why)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_factoryHooked.load() || StandDownIfOwned("factory_hooks"))
                return;
            const HMODULE dxgi = GetModuleHandleW(SystemPath(L"dxgi.dll").c_str());
            if (!dxgi)
                return; // the watcher tries again when the game loads it
            g_factoryHooked = true;
            auto create = reinterpret_cast<PFN_CreateFactory1>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
            IDXGIFactory2* factory = nullptr;
            if (!create || FAILED(create(IID_PPV_ARGS(&factory))) || !factory)
            {
                HOST_ERROR("HOST_D3D12 failed reason=dummy_factory");
                return;
            }
            void* createSwapChain = VtableEntry(factory, kFactoryCreateSwapChain);
            void* createForHwnd = VtableEntry(factory, kFactoryCreateSwapChainForHwnd);
            factory->Release();
            if (!MinHookReady())
                return;
            HOST_LOG("HOST_D3D12 factory_hooks why=%s", why);
            Hook(createSwapChain, reinterpret_cast<void*>(&DetourCreateSwapChain), g_createSwapChain, "IDXGIFactory::CreateSwapChain");
            Hook(createForHwnd, reinterpret_cast<void*>(&DetourCreateSwapChainForHwnd), g_createSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
        }
    }

    namespace
    {
        void GuardedFactoryHooks(const char* why)
        {
            __try
            {
                InstallFactoryHooks(why);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                g_disabled = true;
                HOST_ERROR("HOST_D3D12 failed reason=exception_in_factory_hooks code=0x%08lX (host off for this session)", GetExceptionCode());
            }
        }
    }

    HRESULT WINAPI DetourD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** out)
    {
        LogEnter(g_enterCreateDevice, "D3D12CreateDevice");
        const HRESULT hr = g_createDevice(adapter, level, riid, out);
        if (SUCCEEDED(hr) && out && *out && !g_factoryHooked.load() && !g_disabled.load() && !g_standingDown.load())
        {
            HOST_LOG("HOST_D3D12 device_created level=0x%X", static_cast<unsigned>(level));
            GuardedFactoryHooks("device_created");
        }
        return hr;
    }

    namespace
    {
        bool InstallDeviceHook(HMODULE d3d12)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_deviceHooked.load())
                return true;
            if (StandDownIfOwned("device_hook"))
                return false;
            g_deviceHooked = true;
            if (!MinHookReady())
                return false;
            return Hook(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")), reinterpret_cast<void*>(&DetourD3D12CreateDevice), g_createDevice,
                "D3D12CreateDevice");
        }

        DWORD WINAPI Watch(void*)
        {
            const std::wstring d3d12Path = SystemPath(L"d3d12.dll");
            const std::wstring dxgiPath = SystemPath(L"dxgi.dll");
            // 20 ms steps for up to 15 minutes (F0: the ASI is in the process
            // ~13 s before the game's first device). The factory hooks go in
            // from the D3D12CreateDevice detour (the game's device made: a
            // ReShade loaded by then is seen and nothing is hooked); only if no
            // device came through the hook kDeviceWaitSteps after d3d12.dll
            // was hooked (a device made before the hook) does the watcher put
            // them in itself. They stay pass-through until a swapchain is made
            // on a D3D12 queue (in Vulkan mode the game loads d3d12/dxgi too
            // and never makes one).
            constexpr int kDeviceWaitSteps = 15 * 50;
            int deviceHookedStep = -1;
            for (int step = 0; step < 15 * 60 * 50; ++step)
            {
                if (StandDownIfOwned("watcher"))
                    return 0;
                if (!g_deviceHooked.load())
                {
                    if (const HMODULE d3d12 = GetModuleHandleW(d3d12Path.c_str()))
                    {
                        HOST_LOG("HOST_D3D12 d3d12_loaded step=%d", step);
                        if (!InstallDeviceHook(d3d12) && g_standingDown.load())
                            return 0;
                        deviceHookedStep = step;
                    }
                }
                if (g_disabled.load())
                    return 0;
                if (g_deviceHooked.load() && !g_factoryHooked.load() && deviceHookedStep >= 0 && step - deviceHookedStep >= kDeviceWaitSteps &&
                    GetModuleHandleW(dxgiPath.c_str()))
                    GuardedFactoryHooks("no_device_through_hook_in_15s");
                if (g_factoryHooked.load())
                    return 0;
                Sleep(20);
            }
            HOST_LOG("HOST_D3D12 watcher_done reason=d3d12_never_loaded");
            return 0;
        }
    }

    bool InstallD3D12()
    {
        if (!HostModeIs(L"auto") && !HostModeIs(L"native"))
        {
            HOST_LOG("HOST_D3D12 not_enabled reason=host_mode_unset (Host=reshade, or the ASI did not enable the host)");
            return true;
        }
        if (g_watcherStarted.exchange(true))
            return true;
        // Only CreateThread here: callable from DllMain (the thread starts
        // after the loader lock is released).
        HANDLE thread = CreateThread(nullptr, 0, &Watch, nullptr, 0, nullptr);
        if (!thread)
        {
            g_watcherStarted = false;
            return false;
        }
        CloseHandle(thread);
        return true;
    }
}

extern "C" __declspec(dllexport) int __stdcall RedemptionEditorHostInstallD3D12()
{
    return host::d3d12::InstallD3D12() ? 1 : 0;
}
