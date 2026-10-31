#pragma once

// One editor per process: the ReShade add-on and the host both run the core,
// so whoever starts first takes a named mutex and the other stands down.
// The add-on takes it when ReShade loads it (before any present); the host
// on the first present of the game's swapchain. kernel32 only (the add-on
// calls it from DllMain).

#include <Windows.h>

#include <cwchar>

namespace host
{
    // Local\RedemptionEditorHostOwner_<pid>
    inline void OwnerMutexName(wchar_t (&out)[64])
    {
        swprintf_s(out, L"Local\\RedemptionEditorHostOwner_%lu", GetCurrentProcessId());
    }

    // True: this caller owns the editor now (the handle is kept open until
    // ReleaseOwner). False: someone else in this process already does.
    inline bool TakeOwner(HANDLE& held)
    {
        if (held)
            return true;
        wchar_t name[64];
        OwnerMutexName(name);
        HANDLE mutex = CreateMutexW(nullptr, FALSE, name);
        if (!mutex)
            return false;
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(mutex);
            return false;
        }
        held = mutex;
        return true;
    }

    inline void ReleaseOwner(HANDLE& held)
    {
        if (held)
            CloseHandle(held);
        held = nullptr;
    }
}
