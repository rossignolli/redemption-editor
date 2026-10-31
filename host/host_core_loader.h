#pragma once

// Loads RedemptionEditorCore.dll as a copy (so the file can be replaced while
// the game runs) and swaps the copy between two frames when the file changes.
// Shared by the ReShade add-on (editor/editor_loader.cpp) and the host
// (host/host_runtime.cpp); moved out of editor_loader.cpp unchanged.

#include <imgui.h>
#include <reshade.hpp>

#include <Windows.h>

#include <string>

namespace host
{
    class CoreLoader
    {
    public:
        using LogFn = void (*)(const char* text);

        // `corePath`: the installed RedemptionEditorCore.dll. `table`: the ImGui
        // the core draws with. `apiModule`: the module the core's reshade::log and
        // ReShadeVersion lookups go to (the host DLL); nullptr under ReShade,
        // where the core finds ReShade itself. A core without the
        // EditorCoreSetApiModule export is refused when `apiModule` is set.
        void Init(const std::wstring& corePath, const imgui_function_table* table, HMODULE apiModule, LogFn log);

        // About twice a second (every 30th call): a new write time is taken once
        // it held still for one poll, so a copy in progress is not loaded half
        // written. Call once per frame before Overlay.
        void Poll();

        void Overlay(reshade::api::effect_runtime* runtime);
        void Present(reshade::api::effect_runtime* runtime);
        void DestroyRuntime(reshade::api::effect_runtime* runtime);
        void Unload();

        bool Loaded() const { return m_core.module != nullptr; }
        unsigned Generation() const { return m_generation; }
        unsigned Frame() const { return m_frame; }

    private:
        using StartFn = bool (*)(const imgui_function_table*);
        using RuntimeFn = void (*)(reshade::api::effect_runtime*);
        using StopFn = void (*)();
        using ApiModuleFn = void (*)(HMODULE);

        struct Core
        {
            HMODULE module = nullptr;
            std::wstring copyPath;
            RuntimeFn overlay = nullptr;
            RuntimeFn present = nullptr;
            RuntimeFn destroyRuntime = nullptr;
            StopFn stop = nullptr;
        };

        bool WriteTime(FILETIME& out) const;
        bool Load();
        void Log(const char* text) const;

        Core m_core;
        std::wstring m_corePath;
        const imgui_function_table* m_table = nullptr;
        HMODULE m_apiModule = nullptr;
        LogFn m_log = nullptr;
        FILETIME m_loadedWrite{};  // its write time when loaded
        FILETIME m_pendingWrite{}; // a newer write time seen on the last poll
        unsigned m_frame = 0;
        unsigned m_generation = 0;
    };
}
