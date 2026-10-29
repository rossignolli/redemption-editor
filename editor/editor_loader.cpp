// ReShade add-on that hosts the Redemption Editor. The editor itself is
// RedemptionEditorCore.dll next to this add-on; the add-on loads a copy of it
// (so the file can be replaced while the game runs), forwards the ReShade
// events, and when the file changes swaps the copy between two frames.
// ReShade reads add-ons only at startup: this one is installed with the game
// closed, the core can then be replaced at any time.

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H

#include <imgui.h>
#include <reshade.hpp>

#include "gpu_events.h"
#include "mod_identity.h" // RedemptionEditor.ini (old ReplayProbe.ini read too)

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <string>

namespace
{
    using StartFn = bool (*)(const imgui_function_table*);
    using RuntimeFn = void (*)(reshade::api::effect_runtime*);
    using StopFn = void (*)();

    struct Core
    {
        HMODULE module = nullptr;
        std::wstring copyPath;
        RuntimeFn overlay = nullptr;
        RuntimeFn present = nullptr;
        RuntimeFn destroyRuntime = nullptr;
        StopFn stop = nullptr;
        replay::editor::gpu::GetTableFn gpuEvents = nullptr; // optional
    };

    Core g_core;
    std::wstring g_corePath;       // the installed core, next to this add-on
    FILETIME g_loadedWrite{};      // its write time when loaded
    FILETIME g_pendingWrite{};     // a newer write time seen on the last poll
    unsigned g_frame = 0;
    unsigned g_generation = 0;

    // Application events forwarded to the core (gpu_events.h). The table is
    // published after the core starts and withdrawn before it stops;
    // g_gpuInFlight counts the calls inside it.
    bool g_gpuEvents = false;
    std::atomic<const replay::editor::gpu::EventTable*> g_gpuTable{nullptr};
    std::atomic<int> g_gpuInFlight{0};

    struct GpuCall
    {
        const replay::editor::gpu::EventTable* table;
        GpuCall()
        {
            g_gpuInFlight.fetch_add(1, std::memory_order_seq_cst);
            table = g_gpuTable.load(std::memory_order_seq_cst);
        }
        ~GpuCall() { g_gpuInFlight.fetch_sub(1, std::memory_order_seq_cst); }
    };

    void WithdrawGpuTable()
    {
        g_gpuTable.store(nullptr, std::memory_order_seq_cst);
        for (int spin = 0; g_gpuInFlight.load(std::memory_order_seq_cst) > 0; ++spin)
            Sleep(spin < 64 ? 0 : 1);
    }

    bool WriteTime(FILETIME& out)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(g_corePath.c_str(), GetFileExInfoStandard, &data))
            return false;
        out = data.ftLastWriteTime;
        return true;
    }

    bool Same(const FILETIME& a, const FILETIME& b)
    {
        return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
    }

    void Log(const char* text)
    {
        reshade::log::message(reshade::log::level::info, text);
    }

    void Unload()
    {
        if (!g_core.module)
            return;
        WithdrawGpuTable();
        if (g_core.stop)
            g_core.stop();
        FreeLibrary(g_core.module);
        DeleteFileW(g_core.copyPath.c_str());
        g_core = Core{};
    }

    bool Load()
    {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring folder = std::wstring(temp) + L"RedemptionEditor";
        CreateDirectoryW(folder.c_str(), nullptr);
        wchar_t name[64]{};
        swprintf_s(name, L"\\core-%lu-%u.dll", GetCurrentProcessId(), ++g_generation);
        const std::wstring copy = folder + name;
        if (!CopyFileW(g_corePath.c_str(), copy.c_str(), FALSE))
        {
            Log("RedemptionEditor: could not copy RedemptionEditorCore.dll");
            return false;
        }
        HMODULE module = LoadLibraryW(copy.c_str());
        if (!module)
        {
            DeleteFileW(copy.c_str());
            Log("RedemptionEditor: could not load RedemptionEditorCore.dll");
            return false;
        }
        const auto start = reinterpret_cast<StartFn>(GetProcAddress(module, "EditorCoreStart"));
        Core core;
        core.module = module;
        core.copyPath = copy;
        core.overlay = reinterpret_cast<RuntimeFn>(GetProcAddress(module, "EditorCoreOverlay"));
        core.present = reinterpret_cast<RuntimeFn>(GetProcAddress(module, "EditorCorePresent"));
        core.destroyRuntime = reinterpret_cast<RuntimeFn>(GetProcAddress(module, "EditorCoreDestroyRuntime"));
        core.stop = reinterpret_cast<StopFn>(GetProcAddress(module, "EditorCoreStop"));
        core.gpuEvents = reinterpret_cast<replay::editor::gpu::GetTableFn>(GetProcAddress(module, "EditorCoreGpuEvents"));
        if (!start || !core.overlay || !core.present || !core.destroyRuntime || !core.stop ||
            !start(imgui_function_table_instance()))
        {
            FreeLibrary(module);
            DeleteFileW(copy.c_str());
            Log("RedemptionEditor: RedemptionEditorCore.dll does not export the editor entry points");
            return false;
        }
        g_core = core;
        if (g_gpuEvents && core.gpuEvents)
            g_gpuTable.store(core.gpuEvents(), std::memory_order_seq_cst);
        char text[96];
        std::snprintf(text, sizeof(text), "RedemptionEditor: core loaded (generation %u)", g_generation);
        Log(text);
        return true;
    }

    // About twice a second: a new write time is taken once it held still
    // for one poll, so a copy in progress is not loaded half written.
    // An old ReplayEditor.addon64 loaded too (ReShade loads it after this one:
    // names sort "Red" before "Rep"). Both would draw an editor; this one
    // stays off and says why in ReShade.log.
    bool g_refused = false;
    bool g_attached = false; // false: DllMain refused the attach, the detach has nothing to undo

    void PollCore()
    {
        if (g_refused)
            return;
        if (g_frame == 0 && GetModuleHandleW(replay::identity::kLegacyAddonFileW))
        {
            g_refused = true;
            Log("RedemptionEditor: NOT started, the old ReplayEditor.addon64 is also loaded. Delete ReplayProbe.asi, "
                "ReplayEditor.addon64 and ReplayEditorCore.dll from the game folder and restart the game.");
            return;
        }
        if ((g_frame++ % 30) != 0)
            return;
        FILETIME now{};
        if (!WriteTime(now))
            return;
        if (g_core.module && Same(now, g_loadedWrite))
            return;
        if (!Same(now, g_pendingWrite))
        {
            g_pendingWrite = now;
            if (g_core.module)
                return;
        }
        Unload();
        if (Load())
            g_loadedWrite = now;
        else
            g_loadedWrite = g_pendingWrite = now; // retried when the file changes again
    }

    void OnOverlay(reshade::api::effect_runtime* runtime)
    {
        PollCore();
        if (g_core.overlay)
            g_core.overlay(runtime);
    }

    void OnPresent(reshade::api::effect_runtime* runtime)
    {
        if (g_core.present)
            g_core.present(runtime);
    }

    void OnDestroyRuntime(reshade::api::effect_runtime* runtime)
    {
        if (g_core.destroyRuntime)
            g_core.destroyRuntime(runtime);
    }
}

namespace
{
    using namespace reshade::api;

    bool OnCreateResource(device* d, resource_desc& desc, subresource_data* data, resource_usage usage)
    {
        GpuCall call;
        return call.table && call.table->createResource && call.table->createResource(d, desc, data, usage);
    }
    void OnInitResource(device* d, const resource_desc& desc, const subresource_data* data, resource_usage usage, resource r)
    {
        GpuCall call;
        if (call.table && call.table->initResource)
            call.table->initResource(d, desc, data, usage, r);
    }
    void OnDestroyResource(device* d, resource r)
    {
        GpuCall call;
        if (call.table && call.table->destroyResource)
            call.table->destroyResource(d, r);
    }
    void OnInitResourceView(device* d, resource r, resource_usage usage, const resource_view_desc& desc, resource_view v)
    {
        GpuCall call;
        if (call.table && call.table->initResourceView)
            call.table->initResourceView(d, r, usage, desc, v);
    }
    void OnDestroyResourceView(device* d, resource_view v)
    {
        GpuCall call;
        if (call.table && call.table->destroyResourceView)
            call.table->destroyResourceView(d, v);
    }
    bool OnUpdateDescriptorTables(device* d, uint32_t count, const descriptor_table_update* updates)
    {
        GpuCall call;
        return call.table && call.table->updateDescriptorTables && call.table->updateDescriptorTables(d, count, updates);
    }
    void OnBindDescriptorTables(command_list* c, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count, const descriptor_table* tables)
    {
        GpuCall call;
        if (call.table && call.table->bindDescriptorTables)
            call.table->bindDescriptorTables(c, stages, layout, first, count, tables);
    }
    void OnBindPipeline(command_list* c, pipeline_stage stages, pipeline p)
    {
        GpuCall call;
        if (call.table && call.table->bindPipeline)
            call.table->bindPipeline(c, stages, p);
    }
    bool OnDispatch(command_list* c, uint32_t x, uint32_t y, uint32_t z)
    {
        GpuCall call;
        return call.table && call.table->dispatch && call.table->dispatch(c, x, y, z);
    }
    bool OnCopyTextureToBuffer(command_list* c, resource src, uint32_t sub, const subresource_box* box, resource dst, uint64_t offset, uint32_t rowLength, uint32_t sliceHeight)
    {
        GpuCall call;
        return call.table && call.table->copyTextureToBuffer && call.table->copyTextureToBuffer(c, src, sub, box, dst, offset, rowLength, sliceHeight);
    }
    bool OnCopyTextureRegion(command_list* c, resource src, uint32_t srcSub, const subresource_box* srcBox, resource dst, uint32_t dstSub, const subresource_box* dstBox, filter_mode filter)
    {
        GpuCall call;
        return call.table && call.table->copyTextureRegion && call.table->copyTextureRegion(c, src, srcSub, srcBox, dst, dstSub, dstBox, filter);
    }
    bool OnCopyResource(command_list* c, resource src, resource dst)
    {
        GpuCall call;
        return call.table && call.table->copyResource && call.table->copyResource(c, src, dst);
    }
    void OnBarrier(command_list* c, uint32_t count, const resource* resources, const resource_usage* before, const resource_usage* after)
    {
        GpuCall call;
        if (call.table && call.table->barrier)
            call.table->barrier(c, count, resources, before, after);
    }
    void OnExecuteCommandList(command_queue* q, command_list* c)
    {
        GpuCall call;
        if (call.table && call.table->executeCommandList)
            call.table->executeCommandList(q, c);
    }
    void OnPresentQueue(command_queue* q, swapchain* sc, const rect* source, const rect* dest, uint32_t dirtyCount, const rect* dirty)
    {
        GpuCall call;
        if (call.table && call.table->present)
            call.table->present(q, sc, source, dest, dirtyCount, dirty);
    }

    // The list both for registering and unregistering.
    template <bool Register>
    void GpuEvents()
    {
        using reshade::addon_event;
#define REPLAY_GPU_EVENT(ev, fn) \
        if constexpr (Register) reshade::register_event<addon_event::ev>(fn); else reshade::unregister_event<addon_event::ev>(fn)
        REPLAY_GPU_EVENT(create_resource, OnCreateResource);
        REPLAY_GPU_EVENT(init_resource, OnInitResource);
        REPLAY_GPU_EVENT(destroy_resource, OnDestroyResource);
        REPLAY_GPU_EVENT(init_resource_view, OnInitResourceView);
        REPLAY_GPU_EVENT(destroy_resource_view, OnDestroyResourceView);
        REPLAY_GPU_EVENT(update_descriptor_tables, OnUpdateDescriptorTables);
        REPLAY_GPU_EVENT(bind_descriptor_tables, OnBindDescriptorTables);
        REPLAY_GPU_EVENT(bind_pipeline, OnBindPipeline);
        REPLAY_GPU_EVENT(dispatch, OnDispatch);
        REPLAY_GPU_EVENT(copy_texture_to_buffer, OnCopyTextureToBuffer);
        REPLAY_GPU_EVENT(copy_texture_region, OnCopyTextureRegion);
        REPLAY_GPU_EVENT(copy_resource, OnCopyResource);
        REPLAY_GPU_EVENT(barrier, OnBarrier);
        REPLAY_GPU_EVENT(execute_command_list, OnExecuteCommandList);
        REPLAY_GPU_EVENT(present, OnPresentQueue);
#undef REPLAY_GPU_EVENT
    }

    // RedemptionEditor.ini (or an old ReplayProbe.ini) sits in the game folder (the working directory).
    bool GpuEventsWanted()
    {
        wchar_t folder[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, folder);
        const std::wstring ini = replay::identity::IniPathInW(folder);
        return replay::identity::DevIniIntW(L"EditorGpuEvents", 0, ini.c_str()) != 0; // a developer key
    }

}

extern "C" __declspec(dllexport) const char* NAME = "Redemption Editor";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Replay editor for Red Dead Redemption 2: clip gallery, playback and camera editing.";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        if (!reshade::register_addon(module))
            return FALSE;
        {
            // One copy of the add-on per process (a second
            // RedemptionEditor.addon64, or the old ReplayEditor.addon64
            // loaded before this one).
            const wchar_t* other = L"";
            if (replay::identity::AnotherAddonLoaded(other))
            {
                char text[256];
                std::snprintf(text, sizeof(text),
                    "RedemptionEditor: NOT started, %ls is also loaded. Delete the old ReplayEditor.addon64 and "
                    "ReplayEditorCore.dll from the game folder and restart the game.", other);
                Log(text);
                reshade::unregister_addon(module);
                return FALSE;
            }
        }
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        g_corePath = path;
        g_corePath = g_corePath.substr(0, g_corePath.find_last_of(L'\\') + 1) + replay::identity::kCoreFileW;
        g_attached = true;
        reshade::register_event<reshade::addon_event::reshade_overlay>(OnOverlay);
        reshade::register_event<reshade::addon_event::reshade_present>(OnPresent);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyRuntime);
        g_gpuEvents = GpuEventsWanted();
        if (g_gpuEvents)
        {
            GpuEvents<true>();
            Log("RedemptionEditor: GPU events forwarded to the core (EditorGpuEvents=1)");
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        if (!g_attached)
            break;
        if (g_gpuEvents)
        {
            // At process exit other threads are gone mid-call: no waiting.
            if (reserved)
                g_gpuTable.store(nullptr, std::memory_order_seq_cst);
            else
                WithdrawGpuTable();
            GpuEvents<false>();
        }
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyRuntime);
        reshade::unregister_event<reshade::addon_event::reshade_present>(OnPresent);
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(OnOverlay);
        // At process exit (reserved set) the core is left to the OS: no
        // FreeLibrary under the loader lock of a dying process.
        if (!reserved)
            Unload();
        reshade::unregister_addon(module);
        replay::identity::ReleaseCopyMutex();
        break;
    }
    return TRUE;
}
