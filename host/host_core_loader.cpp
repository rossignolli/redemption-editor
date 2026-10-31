// The core loader shared by the ReShade add-on and the host (host_core_loader.h).

#include "host_core_loader.h"

#include <cstdio>

namespace host
{
    namespace
    {
        bool Same(const FILETIME& a, const FILETIME& b)
        {
            return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
        }
    }

    void CoreLoader::Init(const std::wstring& corePath, const imgui_function_table* table, HMODULE apiModule, LogFn log)
    {
        m_corePath = corePath;
        m_table = table;
        m_apiModule = apiModule;
        m_log = log;
    }

    void CoreLoader::Log(const char* text) const
    {
        if (m_log)
            m_log(text);
    }

    bool CoreLoader::WriteTime(FILETIME& out) const
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(m_corePath.c_str(), GetFileExInfoStandard, &data))
            return false;
        out = data.ftLastWriteTime;
        return true;
    }

    void CoreLoader::Unload()
    {
        if (!m_core.module)
            return;
        if (m_core.stop)
            m_core.stop();
        FreeLibrary(m_core.module);
        DeleteFileW(m_core.copyPath.c_str());
        m_core = Core{};
    }

    bool CoreLoader::Load()
    {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring folder = std::wstring(temp) + L"RedemptionEditor";
        CreateDirectoryW(folder.c_str(), nullptr);
        wchar_t name[64]{};
        swprintf_s(name, L"\\core-%lu-%u.dll", GetCurrentProcessId(), ++m_generation);
        const std::wstring copy = folder + name;
        if (!CopyFileW(m_corePath.c_str(), copy.c_str(), FALSE))
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
        if (m_apiModule)
        {
            // Under the host the core's reshade::log and ReShadeVersion lookups
            // must reach the host DLL, not a ReShade that may also be loaded:
            // set before EditorCoreStart (its first log line).
            const auto setApi = reinterpret_cast<ApiModuleFn>(GetProcAddress(module, "EditorCoreSetApiModule"));
            if (!setApi)
            {
                FreeLibrary(module);
                DeleteFileW(copy.c_str());
                Log("HOST_CORE failed reason=core_without_EditorCoreSetApiModule (core older than the host)");
                return false;
            }
            setApi(m_apiModule);
        }
        if (!start || !core.overlay || !core.present || !core.destroyRuntime || !core.stop || !start(m_table))
        {
            FreeLibrary(module);
            DeleteFileW(copy.c_str());
            Log("RedemptionEditor: RedemptionEditorCore.dll does not export the editor entry points");
            return false;
        }
        m_core = core;
        char text[96];
        std::snprintf(text, sizeof(text), "RedemptionEditor: core loaded (generation %u)", m_generation);
        Log(text);
        return true;
    }

    void CoreLoader::Poll()
    {
        if ((m_frame++ % 30) != 0)
            return;
        FILETIME now{};
        if (!WriteTime(now))
            return;
        if (m_core.module && Same(now, m_loadedWrite))
            return;
        if (!Same(now, m_pendingWrite))
        {
            m_pendingWrite = now;
            if (m_core.module)
                return;
        }
        Unload();
        if (Load())
            m_loadedWrite = now;
        else
            m_loadedWrite = m_pendingWrite = now; // retried when the file changes again
    }

    void CoreLoader::Overlay(reshade::api::effect_runtime* runtime)
    {
        if (m_core.overlay)
            m_core.overlay(runtime);
    }

    void CoreLoader::Present(reshade::api::effect_runtime* runtime)
    {
        if (m_core.present)
            m_core.present(runtime);
    }

    void CoreLoader::DestroyRuntime(reshade::api::effect_runtime* runtime)
    {
        if (m_core.destroyRuntime)
            m_core.destroyRuntime(runtime);
    }
}
