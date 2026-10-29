#pragma once

// The [Keys] section of RedemptionEditor.ini, read into replay::keys::Current().
// The core and the add-on both call LoadFromIni with the same file, so both
// sides hold the same table.

#include "editor_keys.h"
#include "mod_identity.h" // the player's settings file over the game folder's

#include <windows.h>

namespace replay::keys
{
    constexpr char kIniSection[] = "Keys";

    inline LoadResult LoadFromIni(const char* path)
    {
        LoadResult result = Load([path](const char* key, bool& present) {
            // A sentinel default tells "absent" from "present and empty".
            char value[128]{};
            replay::identity::ProfileStringA(kIniSection, key, "\x01", value, sizeof(value), path);
            present = !(value[0] == '\x01' && value[1] == '\0');
            return present ? std::string(value) : std::string();
        });
        Current() = result.table;
        return result;
    }

    inline LoadResult LoadFromIni(const wchar_t* path)
    {
        char narrow[MAX_PATH * 2]{};
        WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
        return LoadFromIni(narrow);
    }
}
