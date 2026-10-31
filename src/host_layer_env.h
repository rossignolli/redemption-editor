#pragma once

// RedemptionEditor.asi turns the host on for Vulkan: from its DllMain (the
// ASI loader loads it ~13 s before the game creates its Vulkan instance, as
// measured in game) it adds the host's manifest folder
// (<game>\RedemptionEditor\host) to VK_ADD_LAYER_PATH and the layer to
// VK_LOADER_LAYERS_ENABLE, in this process's environment only (nothing in the
// registry, nothing outside the game). The Vulkan loader then loads
// RedemptionEditorHost.dll as the layer VK_LAYER_redemption_editor.
//
// kernel32 only: DllMain runs under the loader lock.
//
// [RedemptionEditor] Host = auto | native | reshade (default auto):
//   reshade: no layer (the editor runs only through the ReShade add-on);
//   auto:    the layer; the first of the add-on (at its load) and the host
//            (at the first present) to take the owner mutex runs the editor;
//   native:  the layer, and this DllMain takes the owner mutex on the host's
//            behalf (before ReShade can load add-ons: the add-on stands down).
// The mode reaches the layer in REDEMPTION_EDITOR_HOST_MODE.
// The Host key (ini_public.h, the shipped ini) is read here with
// GetPrivateProfileStringW directly: DllMain runs before the ini layer exists.
// Developer build only: HostBenchNoReShade=1 also turns ReShade's global Vulkan
// layer off for this process (its manifest's disable_environment), so the bench
// can test a machine without ReShade without editing C:\ProgramData (admin).

#include "../host/host_owner.h"

#include <Windows.h>

#include <cwchar>

namespace replay::host_layer
{
    inline constexpr wchar_t kLayerName[] = L"VK_LAYER_redemption_editor";
    inline constexpr wchar_t kModeVariable[] = L"REDEMPTION_EDITOR_HOST_MODE";

    // `list` already holds `item` as one whole entry of the `separator` list.
    inline bool ListHas(const wchar_t* list, const wchar_t* item, wchar_t separator)
    {
        const size_t length = wcslen(item);
        for (const wchar_t* at = list; at && *at;)
        {
            const wchar_t* end = wcschr(at, separator);
            const size_t size = end ? static_cast<size_t>(end - at) : wcslen(at);
            if (size == length && _wcsnicmp(at, item, length) == 0)
                return true;
            at = end ? end + 1 : nullptr;
        }
        return false;
    }

    // Appends `item` to the variable's `separator` list (unless it is there).
    inline bool AppendToVariable(const wchar_t* name, const wchar_t* item, wchar_t separator)
    {
        wchar_t current[8192]{};
        const DWORD size = GetEnvironmentVariableW(name, current, static_cast<DWORD>(sizeof(current) / sizeof(current[0])));
        if (size >= sizeof(current) / sizeof(current[0]))
            return false; // too long to extend safely: left as it is
        if (ListHas(current, item, separator))
            return true;
        wchar_t value[8192 + MAX_PATH + 2]{};
        if (size == 0)
            wcscpy_s(value, item);
        else
            swprintf_s(value, L"%s%c%s", current, separator, item);
        return SetEnvironmentVariableW(name, value) != FALSE;
    }

    // From RedemptionEditor.asi's DllMain. `module`: the ASI.
    inline void Enable(HMODULE module)
    {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return;
        wchar_t* slash = wcsrchr(path, L'\\');
        if (!slash)
            return;
        *slash = L'\0'; // the game folder

        wchar_t ini[MAX_PATH]{};
        swprintf_s(ini, L"%s\\RedemptionEditor.ini", path);
        wchar_t mode[32]{};
        GetPrivateProfileStringW(L"RedemptionEditor", L"Host", L"auto", mode, 32, ini);
#ifdef REDEMPTION_DEV
        if (GetPrivateProfileIntW(L"RedemptionEditor", L"HostBenchNoReShade", 0, ini) == 1)
            SetEnvironmentVariableW(L"DISABLE_VK_LAYER_reshade_1", L"1");
#endif
        if (_wcsicmp(mode, L"reshade") == 0)
            return;
        if (_wcsicmp(mode, L"native") != 0)
            wcscpy_s(mode, L"auto");

        wchar_t folder[MAX_PATH]{};
        swprintf_s(folder, L"%s\\RedemptionEditor\\host", path);
        wchar_t manifest[MAX_PATH]{};
        swprintf_s(manifest, L"%s\\RedemptionEditorHost.json", folder);
        if (GetFileAttributesW(manifest) == INVALID_FILE_ATTRIBUTES)
            return; // the host is not installed: ReShade only, as before

        SetEnvironmentVariableW(kModeVariable, mode);
        if (_wcsicmp(mode, L"native") == 0)
        {
            HANDLE owner = nullptr; // kept for the process's life
            host::TakeOwner(owner);
        }
        if (AppendToVariable(L"VK_ADD_LAYER_PATH", folder, L';'))
            AppendToVariable(L"VK_LOADER_LAYERS_ENABLE", kLayerName, L',');
    }
}
