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
#include "../host/host_core_loader.h"
#include "../host/host_owner.h"

#include <Windows.h>
#include <cstdio>
#include <string>

namespace
{
    host::CoreLoader g_loader; // the copy and hot swap (host/host_core_loader.h)

    void Log(const char* text)
    {
        reshade::log::message(reshade::log::level::info, text);
    }

    // An old ReplayEditor.addon64 loaded too (ReShade loads it after this one:
    // names sort "Red" before "Rep"). Both would draw an editor; this one
    // stays off and says why in ReShade.log.
    bool g_refused = false;
    bool g_attached = false; // false: DllMain refused the attach, the detach has nothing to undo
    HANDLE g_ownerMutex = nullptr; // host::kOwnerMutex: this add-on runs the editor in this process

    void PollCore()
    {
        if (g_refused)
            return;
        if (g_loader.Frame() == 0 && GetModuleHandleW(replay::identity::kLegacyAddonFileW))
        {
            g_refused = true;
            Log("RedemptionEditor: NOT started, the old ReplayEditor.addon64 is also loaded. Delete ReplayProbe.asi, "
                "ReplayEditor.addon64 and ReplayEditorCore.dll from the game folder and restart the game.");
            return;
        }
        g_loader.Poll();
    }

    void OnOverlay(reshade::api::effect_runtime* runtime)
    {
        PollCore();
        g_loader.Overlay(runtime);
    }

    void OnPresent(reshade::api::effect_runtime* runtime)
    {
        g_loader.Present(runtime);
    }

    void OnDestroyRuntime(reshade::api::effect_runtime* runtime)
    {
        g_loader.DestroyRuntime(runtime);
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
        // One editor per process: the host (RedemptionEditorHost.dll, a Vulkan
        // layer) may run it instead of ReShade. Taken here, at load, which is
        // before any present: the host asks on its first present and stands
        // down while this add-on holds the name.
        if (!host::TakeOwner(g_ownerMutex))
        {
            Log("RedemptionEditor: NOT started, the Redemption Editor host already runs the editor in this process "
                "(RedemptionEditor.ini [RedemptionEditor] Host=reshade gives it to ReShade).");
            replay::identity::ReleaseCopyMutex();
            reshade::unregister_addon(module);
            return FALSE;
        }
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        std::wstring corePath = path;
        corePath = corePath.substr(0, corePath.find_last_of(L'\\') + 1) + replay::identity::kCoreFileW;
        g_loader.Init(corePath, imgui_function_table_instance(), nullptr, Log);
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
            g_loader.Unload();
        reshade::unregister_addon(module);
        replay::identity::ReleaseCopyMutex();
        host::ReleaseOwner(g_ownerMutex);
        break;
    }
    return TRUE;
}
