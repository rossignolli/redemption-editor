#pragma once

// The folder of the files that ship with the editor, next to RDR2.exe:
//   RedemptionEditor\dlls\   the FFmpeg libraries of the video export
//   RedemptionEditor\fonts\  the title font (display_font.h)
//   RedemptionEditor\ui\     the painted art (ui_art.h)
// Found from the module path of the process (RDR2.exe), not of the core.

#include <Windows.h>
#include <string>

namespace bundled_files
{
    // "<folder of RDR2.exe>\RedemptionEditor\" (with the trailing backslash).
    inline std::wstring RedemptionEditorFolder()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring folder = exe;
        folder = folder.substr(0, folder.find_last_of(L"\\/") + 1);
        return folder + L"RedemptionEditor\\";
    }
}
