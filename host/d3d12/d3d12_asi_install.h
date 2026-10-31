#pragma once

// For RedemptionEditor.asi (main.cpp): starts the host's D3D12 path. Call it
// from DllMain right after replay::host_layer::Enable (which sets
// REDEMPTION_EDITOR_HOST_MODE; with Host=reshade it is unset and the host's
// install returns without doing anything). kernel32 only and only a thread
// created here, so it is safe under the loader lock: the thread runs once
// DllMain returns, loads <game>\RedemptionEditor\host\RedemptionEditorHost.dll
// (pinned) and calls RedemptionEditorHostInstallD3D12, which only watches for
// the game's own d3d12.dll/dxgi.dll (host/d3d12/d3d12_hooks.h). In Vulkan mode
// the same DLL is the Vulkan layer; the D3D12 hooks then stay pass-through
// (no swapchain is ever made on a D3D12 queue).
// Owned by lane host-dx12; wiring it into main.cpp is the main session's call.

#include <Windows.h>

#include <cwchar>

namespace replay::host_d3d12
{
    inline DWORD WINAPI LoadAndInstall(void* parameter)
    {
        const wchar_t* path = static_cast<const wchar_t*>(parameter);
        HMODULE host = LoadLibraryW(path);
        if (host)
        {
            HMODULE pinned = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, path, &pinned);
            using Install = int(__stdcall*)();
            if (const auto install = reinterpret_cast<Install>(reinterpret_cast<void*>(GetProcAddress(host, "RedemptionEditorHostInstallD3D12"))))
                install();
        }
        HeapFree(GetProcessHeap(), 0, parameter);
        return 0;
    }

    // `asi`: RedemptionEditor.asi's module.
    inline void Start(HMODULE asi)
    {
        wchar_t mode[32]{};
        if (GetEnvironmentVariableW(L"REDEMPTION_EDITOR_HOST_MODE", mode, 32) == 0)
            return; // Host=reshade or no host installed
        wchar_t game[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(asi, game, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return;
        wchar_t* slash = wcsrchr(game, L'\\');
        if (!slash)
            return;
        *slash = L'\0';
        auto* path = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_PATH * sizeof(wchar_t)));
        if (!path)
            return;
        swprintf_s(path, MAX_PATH, L"%s\\RedemptionEditor\\host\\RedemptionEditorHost.dll", game);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        {
            HeapFree(GetProcessHeap(), 0, path);
            return;
        }
        HANDLE thread = CreateThread(nullptr, 0, &LoadAndInstall, path, 0, nullptr);
        if (thread)
            CloseHandle(thread);
        else
            HeapFree(GetProcessHeap(), 0, path);
    }
}
