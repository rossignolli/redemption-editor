// ReShade add-on that hosts the Redemption Editor. The editor itself is
// RedemptionEditorCore.dll next to this add-on; the add-on loads a copy of it
// (so the file can be replaced while the game runs), forwards the ReShade
// events, and when the file changes swaps the copy between two frames.
// ReShade reads add-ons only at startup: this one is installed with the game
// closed, the core can then be replaced at any time.

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H

#include <imgui.h>
#include <reshade.hpp>

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
    };

    Core g_core;
    std::wstring g_corePath;       // the installed core, next to this add-on
    FILETIME g_loadedWrite{};      // its write time when loaded
    FILETIME g_pendingWrite{};     // a newer write time seen on the last poll
    unsigned g_frame = 0;
    unsigned g_generation = 0;

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
        if (!start || !core.overlay || !core.present || !core.destroyRuntime || !core.stop ||
            !start(imgui_function_table_instance()))
        {
            FreeLibrary(module);
            DeleteFileW(copy.c_str());
            Log("RedemptionEditor: RedemptionEditorCore.dll does not export the editor entry points");
            return false;
        }
        g_core = core;
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
        break;
    }
    case DLL_PROCESS_DETACH:
        if (!g_attached)
            break;
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
