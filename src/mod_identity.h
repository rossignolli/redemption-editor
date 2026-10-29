#pragma once

// The names the mod ships and creates: RedemptionEditor.asi (the
// ScriptHookRDR2 script), RedemptionEditor.addon64 (the ReShade add-on, a
// loader), RedemptionEditorCore.dll (the editor it loads), RedemptionEditor.ini,
// RedemptionEditor.log. Older builds used ReplayProbe.asi,
// ReplayEditor.addon64, ReplayEditorCore.dll, ReplayProbe.ini with the section
// [ReplayPoC]: those names stay here only to read an old install's settings
// and to refuse to run next to an old copy of the mod.
//
// The folders the mod creates (ReplayClips, ReplayProjects, ReplayReports,
// ReplayDebug) keep their names: the clips of every earlier build live there.
// They sit in the game folder, or in a folder of the player's when the game
// folder takes no writes (storage_paths.h). RedemptionEditor.ini stays in the
// game folder; the settings the player changes then go to the
// RedemptionEditor.ini of that folder, read over the game folder's.
//
// Header only, used by the ASI (MultiByte), the add-on and the core (Unicode):
// the A and W calls are spelled out.

#include <Windows.h>

#include "storage_paths.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace replay::identity
{
    inline constexpr char kProductName[] = "Redemption Editor";

    inline constexpr char kAsiFile[] = "RedemptionEditor.asi";
    inline constexpr wchar_t kAddonFileW[] = L"RedemptionEditor.addon64";
    inline constexpr wchar_t kCoreFileW[] = L"RedemptionEditorCore.dll";
    inline constexpr char kLogFile[] = "RedemptionEditor.log";
    inline constexpr char kLogStem[] = "RedemptionEditor"; // rotated: RedemptionEditor-<date>.log
    inline constexpr char kCrashLogFile[] = "RedemptionEditor.crash.log";
    inline constexpr char kIniFile[] = "RedemptionEditor.ini";
    inline constexpr wchar_t kIniFileW[] = L"RedemptionEditor.ini";
    inline constexpr char kIniSection[] = "RedemptionEditor";
    inline constexpr wchar_t kIniSectionW[] = L"RedemptionEditor";

    // The names of older builds (read only, never created).
    inline constexpr char kLegacyAsiFile[] = "ReplayProbe.asi";
    inline constexpr wchar_t kLegacyAddonFileW[] = L"ReplayEditor.addon64";
    inline constexpr char kLegacyIniFile[] = "ReplayProbe.ini";
    inline constexpr wchar_t kLegacyIniFileW[] = L"ReplayProbe.ini";
    inline constexpr char kLegacyIniSection[] = "ReplayPoC";
    inline constexpr wchar_t kLegacyIniSectionW[] = L"ReplayPoC";

    inline bool FileExistsA(const std::string& path)
    {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    inline bool FileExistsW(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    // The settings file in `folder` (the game folder): RedemptionEditor.ini;
    // while only an old ReplayProbe.ini exists (an install not migrated yet,
    // or the add-on reading before the script copied it), that one.
    inline std::string IniPathIn(const std::string& folder)
    {
        const std::string current = folder + "\\" + kIniFile;
        if (FileExistsA(current))
            return current;
        const std::string legacy = folder + "\\" + kLegacyIniFile;
        return FileExistsA(legacy) ? legacy : current;
    }

    inline std::wstring IniPathInW(const std::wstring& folder)
    {
        const std::wstring current = folder + L"\\" + kIniFileW;
        if (FileExistsW(current))
            return current;
        const std::wstring legacy = folder + L"\\" + kLegacyIniFileW;
        return FileExistsW(legacy) ? legacy : current;
    }

    // The engine safety switches (ini_public.h kSupportSwitches) are not in
    // the shipped ini; older inis listed them in [Troubleshooting], and a key
    // set there is still read like one of [RedemptionEditor].
    inline constexpr char kTroubleshootingSection[] = "Troubleshooting";
    inline constexpr wchar_t kTroubleshootingSectionW[] = L"Troubleshooting";

    // The section a key is read from: [RedemptionEditor], else the old
    // [ReplayPoC] when only that one has the key, else [Troubleshooting]
    // when only that one has it.
    inline const char* SectionFor(const char* key, const char* path)
    {
        char probe[8]{};
        if (GetPrivateProfileStringA(kIniSection, key, "", probe, sizeof(probe), path) > 0)
            return kIniSection;
        if (GetPrivateProfileStringA(kLegacyIniSection, key, "", probe, sizeof(probe), path) > 0)
            return kLegacyIniSection;
        if (GetPrivateProfileStringA(kTroubleshootingSection, key, "", probe, sizeof(probe), path) > 0)
            return kTroubleshootingSection;
        return kIniSection;
    }

    inline const wchar_t* SectionForW(const wchar_t* key, const wchar_t* path)
    {
        wchar_t probe[8]{};
        if (GetPrivateProfileStringW(kIniSectionW, key, L"", probe, 8, path) > 0)
            return kIniSectionW;
        if (GetPrivateProfileStringW(kLegacyIniSectionW, key, L"", probe, 8, path) > 0)
            return kLegacyIniSectionW;
        if (GetPrivateProfileStringW(kTroubleshootingSectionW, key, L"", probe, 8, path) > 0)
            return kTroubleshootingSectionW;
        return kIniSectionW;
    }

    // ---- The player's settings over the shipped ones ----
    //
    // When the game folder takes no writes (storage_paths.h), the settings the
    // player changes go to the RedemptionEditor.ini of the base folder (the
    // "user" file), and a key there wins over the game folder's. Only the
    // settings file is layered: a read of another file (a test's) is plain.
    // The *Layered calls take the user file (empty: none) for the tests.

    inline std::wstring AsciiW(const char* text)
    {
        std::wstring out;
        for (const char* c = text ? text : ""; *c; ++c)
            out += static_cast<wchar_t>(static_cast<unsigned char>(*c));
        return out;
    }

    inline bool SettingsFileNameW(const std::wstring& path)
    {
        const std::size_t slash = path.find_last_of(L"\\/");
        const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        return storage::SamePath(name, kIniFileW) || storage::SamePath(name, kLegacyIniFileW);
    }

    // The user file layered over `path` ("" when none).
    inline std::wstring UserIniForW(const wchar_t* path)
    {
        if (!path || !SettingsFileNameW(path))
            return {};
        return storage::Current().userIni;
    }

    inline std::wstring UserIniFor(const char* path) { return path ? UserIniForW(storage::Widen(path).c_str()) : std::wstring(); }

    // `key` of `section` is in `file` (present, even empty).
    inline bool HasKeyW(const wchar_t* section, const wchar_t* key, const std::wstring& file)
    {
        if (file.empty())
            return false;
        wchar_t probe[8]{};
        GetPrivateProfileStringW(section, key, L"\x01", probe, 8, file.c_str());
        return !(probe[0] == L'\x01' && probe[1] == L'\0');
    }

    // The section of the user file that holds `key` (the three of SectionFor),
    // or null.
    inline const wchar_t* UserSectionW(const wchar_t* key, const std::wstring& user)
    {
        for (const wchar_t* section : {kIniSectionW, kLegacyIniSectionW, kTroubleshootingSectionW})
            if (HasKeyW(section, key, user))
                return section;
        return nullptr;
    }

    inline UINT IniIntLayered(const char* key, INT fallback, const char* path, const std::wstring& user)
    {
        if (!user.empty())
        {
            const std::wstring wide = AsciiW(key);
            if (const wchar_t* section = UserSectionW(wide.c_str(), user))
                return GetPrivateProfileIntW(section, wide.c_str(), fallback, user.c_str());
        }
        return GetPrivateProfileIntA(SectionFor(key, path), key, fallback, path);
    }

    inline UINT IniIntLayeredW(const wchar_t* key, INT fallback, const wchar_t* path, const std::wstring& user)
    {
        if (!user.empty())
            if (const wchar_t* section = UserSectionW(key, user))
                return GetPrivateProfileIntW(section, key, fallback, user.c_str());
        return GetPrivateProfileIntW(SectionForW(key, path), key, fallback, path);
    }

    inline DWORD IniStringLayered(const char* key, const char* fallback, char* out, DWORD size, const char* path, const std::wstring& user)
    {
        if (!user.empty() && out && size > 0)
        {
            const std::wstring wide = AsciiW(key);
            if (const wchar_t* section = UserSectionW(wide.c_str(), user))
            {
                std::vector<wchar_t> value(size + 1, L'\0');
                GetPrivateProfileStringW(section, wide.c_str(), L"", value.data(), size, user.c_str());
                const std::string text = storage::Ansi(value.data());
                const std::size_t length = (std::min)(text.size(), static_cast<std::size_t>(size - 1));
                std::memcpy(out, text.data(), length);
                out[length] = '\0';
                return static_cast<DWORD>(length);
            }
        }
        return GetPrivateProfileStringA(SectionFor(key, path), key, fallback, out, size, path);
    }

    inline DWORD IniStringLayeredW(const wchar_t* key, const wchar_t* fallback, wchar_t* out, DWORD size, const wchar_t* path,
        const std::wstring& user)
    {
        if (!user.empty())
            if (const wchar_t* section = UserSectionW(key, user))
                return GetPrivateProfileStringW(section, key, L"", out, size, user.c_str());
        return GetPrivateProfileStringW(SectionForW(key, path), key, fallback, out, size, path);
    }

    inline UINT IniInt(const char* key, INT fallback, const char* path) { return IniIntLayered(key, fallback, path, UserIniFor(path)); }

    inline UINT IniIntW(const wchar_t* key, INT fallback, const wchar_t* path)
    {
        return IniIntLayeredW(key, fallback, path, UserIniForW(path));
    }

    inline DWORD IniString(const char* key, const char* fallback, char* out, DWORD size, const char* path)
    {
        return IniStringLayered(key, fallback, out, size, path, UserIniFor(path));
    }

    inline DWORD IniStringW(const wchar_t* key, const wchar_t* fallback, wchar_t* out, DWORD size, const wchar_t* path)
    {
        return IniStringLayeredW(key, fallback, out, size, path, UserIniForW(path));
    }

    // Another section of the settings file ([Export], [Keys]), layered the
    // same way: the user file's value when it has the key.
    inline UINT ProfileIntW(const wchar_t* section, const wchar_t* key, INT fallback, const wchar_t* path)
    {
        const std::wstring user = UserIniForW(path);
        if (HasKeyW(section, key, user))
            return GetPrivateProfileIntW(section, key, fallback, user.c_str());
        return GetPrivateProfileIntW(section, key, fallback, path);
    }

    inline DWORD ProfileStringW(const wchar_t* section, const wchar_t* key, const wchar_t* fallback, wchar_t* out, DWORD size,
        const wchar_t* path)
    {
        const std::wstring user = UserIniForW(path);
        if (HasKeyW(section, key, user))
            return GetPrivateProfileStringW(section, key, fallback, out, size, user.c_str());
        return GetPrivateProfileStringW(section, key, fallback, out, size, path);
    }

    inline UINT ProfileIntA(const char* section, const char* key, INT fallback, const char* path)
    {
        const std::wstring user = UserIniFor(path);
        const std::wstring wideSection = AsciiW(section), wideKey = AsciiW(key);
        if (HasKeyW(wideSection.c_str(), wideKey.c_str(), user))
            return GetPrivateProfileIntW(wideSection.c_str(), wideKey.c_str(), fallback, user.c_str());
        return GetPrivateProfileIntA(section, key, fallback, path);
    }

    inline DWORD ProfileStringLayeredA(const char* section, const char* key, const char* fallback, char* out, DWORD size, const char* path,
        const std::wstring& user)
    {
        const std::wstring wideSection = AsciiW(section), wideKey = AsciiW(key);
        if (out && size > 0 && HasKeyW(wideSection.c_str(), wideKey.c_str(), user))
        {
            std::vector<wchar_t> value(size + 1, L'\0');
            GetPrivateProfileStringW(wideSection.c_str(), wideKey.c_str(), L"", value.data(), size, user.c_str());
            const std::string text = storage::Ansi(value.data());
            const std::size_t length = (std::min)(text.size(), static_cast<std::size_t>(size - 1));
            std::memcpy(out, text.data(), length);
            out[length] = '\0';
            return static_cast<DWORD>(length);
        }
        return GetPrivateProfileStringA(section, key, fallback, out, size, path);
    }

    inline DWORD ProfileStringA(const char* section, const char* key, const char* fallback, char* out, DWORD size, const char* path)
    {
        return ProfileStringLayeredA(section, key, fallback, out, size, path, UserIniFor(path));
    }

    // The developer build (REDEMPTION_DEV, MSBuild /p:RedemptionDev=true) reads
    // every key. The public build reads only the keys of ini_public.h
    // kPublicReadKeys: a developer key (a research probe, a tuning value) takes
    // the code's default there, without opening the file.
#if defined(REDEMPTION_DEV)
    inline constexpr bool kDevBuild = true;
    // The CONFIG_BUILD log field; also the mark tools/package_qa.py looks for
    // in the ASI (a QA package is built from the developer binaries).
    inline constexpr char kBuildFlavor[] = "flavor=dev";
#else
    inline constexpr bool kDevBuild = false;
    inline constexpr char kBuildFlavor[] = "flavor=public";
#endif

    inline UINT DevIniInt(const char* key, INT fallback, const char* path)
    {
#if defined(REDEMPTION_DEV)
        return IniInt(key, fallback, path);
#else
        (void)key;
        (void)path;
        return static_cast<UINT>(fallback); // what GetPrivateProfileIntA returns for a missing key
#endif
    }

    inline UINT DevIniIntW(const wchar_t* key, INT fallback, const wchar_t* path)
    {
#if defined(REDEMPTION_DEV)
        return IniIntW(key, fallback, path);
#else
        (void)key;
        (void)path;
        return static_cast<UINT>(fallback);
#endif
    }

    // As GetPrivateProfileStringA for a missing key: the fallback copied
    // (cut to size - 1), its length returned.
    inline DWORD DevIniString(const char* key, const char* fallback, char* out, DWORD size, const char* path)
    {
#if defined(REDEMPTION_DEV)
        return IniString(key, fallback, out, size, path);
#else
        (void)key;
        (void)path;
        if (!out || size == 0)
            return 0;
        DWORD length = 0;
        const char* text = fallback ? fallback : "";
        while (text[length] && length + 1 < size)
        {
            out[length] = text[length];
            ++length;
        }
        out[length] = '\0';
        return length;
#endif
    }

    // Writes always go to [RedemptionEditor] (read first, so they win over
    // an old [ReplayPoC] value of the same key), of the user file when the
    // game folder takes no writes.
    inline BOOL IniWriteLayered(const char* key, const char* value, const char* path, const std::wstring& user)
    {
        if (!user.empty())
            return WritePrivateProfileStringW(kIniSectionW, AsciiW(key).c_str(), value ? storage::Widen(value).c_str() : nullptr, user.c_str());
        return WritePrivateProfileStringA(kIniSection, key, value, path);
    }

    inline BOOL IniWrite(const char* key, const char* value, const char* path) { return IniWriteLayered(key, value, path, UserIniFor(path)); }

    inline BOOL IniWriteW(const wchar_t* key, const wchar_t* value, const wchar_t* path)
    {
        const std::wstring user = UserIniForW(path);
        return WritePrivateProfileStringW(kIniSectionW, key, value, user.empty() ? path : user.c_str());
    }

    // The [RedemptionEditor] section's "key=value" pairs (NUL separated, as
    // GetPrivateProfileSectionA), or the old [ReplayPoC] one when the new is
    // empty; the user file's pairs over them (its value for a key in both).
    inline DWORD IniSectionLayered(char* out, DWORD size, const char* path, const std::wstring& user)
    {
        DWORD length = GetPrivateProfileSectionA(kIniSection, out, size, path);
        if (length == 0)
            length = GetPrivateProfileSectionA(kLegacyIniSection, out, size, path);
        if (user.empty() || !out || size < 2)
            return length;
        std::vector<std::string> pairs;
        for (DWORD at = 0; at < length && out[at];)
        {
            pairs.emplace_back(out + at);
            at += static_cast<DWORD>(pairs.back().size()) + 1;
        }
        std::vector<wchar_t> wide(32768, L'\0');
        const DWORD userLength = GetPrivateProfileSectionW(kIniSectionW, wide.data(), static_cast<DWORD>(wide.size()), user.c_str());
        for (DWORD at = 0; at < userLength && wide[at];)
        {
            const std::wstring pair(wide.data() + at);
            at += static_cast<DWORD>(pair.size()) + 1;
            const std::string text = storage::Ansi(pair);
            const std::string key = text.substr(0, text.find('='));
            bool replaced = false;
            for (std::string& kept : pairs)
                if (_stricmp(kept.substr(0, kept.find('=')).c_str(), key.c_str()) == 0)
                {
                    kept = text;
                    replaced = true;
                }
            if (!replaced)
                pairs.push_back(text);
        }
        DWORD at = 0;
        for (const std::string& pair : pairs)
        {
            if (at + pair.size() + 2 > size)
                break;
            std::memcpy(out + at, pair.c_str(), pair.size() + 1);
            at += static_cast<DWORD>(pair.size()) + 1;
        }
        out[at] = '\0';
        return at;
    }

    inline DWORD IniSection(char* out, DWORD size, const char* path) { return IniSectionLayered(out, size, path, UserIniFor(path)); }

    // The old settings' text with the section header renamed: "[ReplayPoC]"
    // (any case, spaces around it) becomes "[RedemptionEditor]"; every other
    // line, [Export] included, is kept as it is.
    inline std::string RenameLegacySection(const std::string& text)
    {
        std::string out;
        out.reserve(text.size() + 16);
        std::size_t at = 0;
        while (at < text.size())
        {
            std::size_t end = text.find('\n', at);
            const bool lastLine = end == std::string::npos;
            if (lastLine)
                end = text.size();
            std::string line = text.substr(at, end - at);
            std::string trimmed = line;
            while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' ' || trimmed.back() == '\t'))
                trimmed.pop_back();
            std::size_t first = 0;
            while (first < trimmed.size() && (trimmed[first] == ' ' || trimmed[first] == '\t'))
                ++first;
            trimmed = trimmed.substr(first);
            if (trimmed.size() == 11 && _stricmp(trimmed.c_str(), "[ReplayPoC]") == 0)
                line = std::string("[") + kIniSection + "]" + (!line.empty() && line.back() == '\r' ? "\r" : "");
            out += line;
            if (!lastLine)
                out += '\n';
            at = end + 1;
        }
        return out;
    }

    enum class IniMigration
    {
        None,     // RedemptionEditor.ini exists, or neither file does
        Copied,   // ReplayProbe.ini copied to RedemptionEditor.ini (section renamed)
        Failed,   // ReplayProbe.ini could not be read or the copy not written
    };

    // Once, at the script's load: an install upgraded from an older build
    // keeps its settings. ReplayProbe.ini is left where it is (an old copy of
    // the mod, if still installed, reads it).
    inline IniMigration MigrateLegacyIni(const std::string& folder)
    {
        const std::string current = folder + "\\" + kIniFile;
        const std::string legacy = folder + "\\" + kLegacyIniFile;
        if (FileExistsA(current) || !FileExistsA(legacy))
            return IniMigration::None;
        std::string text;
        {
            HANDLE file = CreateFileA(legacy.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return IniMigration::Failed;
            char buffer[4096];
            DWORD read = 0;
            bool ok = true;
            while ((ok = ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE) && read > 0)
                text.append(buffer, read);
            CloseHandle(file);
            if (!ok)
                return IniMigration::Failed;
        }
        const std::string renamed = RenameLegacySection(text);
        const std::string temporary = current + ".tmp";
        HANDLE file = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return IniMigration::Failed;
        DWORD written = 0;
        const bool wrote = WriteFile(file, renamed.data(), static_cast<DWORD>(renamed.size()), &written, nullptr) != FALSE &&
            written == renamed.size();
        const bool closed = CloseHandle(file) != FALSE;
        if (!wrote || !closed || !MoveFileExA(temporary.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileA(temporary.c_str());
            return IniMigration::Failed;
        }
        return IniMigration::Copied;
    }

    // The first run writes the shipped settings
    // (ini_public.h kShippedIni, every value the code's default) when the
    // game folder has neither RedemptionEditor.ini nor the old
    // ReplayProbe.ini. `text` uses '\n'; the file gets CRLF. False when a
    // settings file is already there or the write failed.
    inline bool WriteDefaultIni(const std::string& folder, const char* text)
    {
        const std::string current = folder + "\\" + kIniFile;
        const std::string legacy = folder + "\\" + kLegacyIniFile;
        if (!text || FileExistsA(current) || FileExistsA(legacy))
            return false;
        std::string body;
        for (const char* c = text; *c; ++c)
        {
            if (*c == '\n' && (c == text || c[-1] != '\r'))
                body += '\r';
            body += *c;
        }
        const std::string temporary = current + ".tmp";
        HANDLE file = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        const bool wrote = WriteFile(file, body.data(), static_cast<DWORD>(body.size()), &written, nullptr) != FALSE &&
            written == body.size();
        const bool closed = CloseHandle(file) != FALSE;
        // No replace: a file that appeared meanwhile (the add-on, another
        // copy) is kept.
        if (!wrote || !closed || !MoveFileExA(temporary.c_str(), current.c_str(), 0))
        {
            DeleteFileA(temporary.c_str());
            return false;
        }
        return true;
    }

    // The state files a reload or a crash leaves in the game folder for the
    // next load (the world to give back, a paused replay's freeze, the last
    // unload's report): an old build's are renamed once, so an upgrade made
    // right after a crash still puts the world back.
    inline int MigrateLegacyStateFiles()
    {
        static const char* const kPairs[][2] = {
            {"ReplayProbe.world-restore", "RedemptionEditor.world-restore"},
            {"ReplayProbe.world-ipl", "RedemptionEditor.world-ipl"},
            {"ReplayProbe.world-freeze", "RedemptionEditor.world-freeze"},
            {"ReplayProbe.unload-report", "RedemptionEditor.unload-report"},
        };
        int moved = 0;
        for (const auto& pair : kPairs)
            if (FileExistsA(pair[0]) && !FileExistsA(pair[1]) && MoveFileExA(pair[0], pair[1], 0))
                ++moved;
        return moved;
    }

    // The mutex of the copy that runs (closed when its module unloads, so a
    // hot reload, Ctrl+R, finds the name free again).
    inline HANDLE g_copyMutex = nullptr;

    inline void ReleaseCopyMutex()
    {
        if (g_copyMutex)
            CloseHandle(g_copyMutex);
        g_copyMutex = nullptr;
    }

    // One copy of the mod per process. The old ReplayProbe.asi does not know
    // this mutex: it is found by its module name. Called by the script before
    // it starts anything; `other` names what was found. The mutex handle is
    // kept for the life of the module.
    // ScriptHook runs ScriptMain again in the same module when the game
    // restarts its scripts (a save loaded, a death, a mission reload). This
    // copy already owns the name then: a restart, not a second copy.
    inline bool AnotherCopyLoaded(const char*& other, bool* restarted = nullptr)
    {
        other = "";
        if (restarted)
            *restarted = false;
        if (g_copyMutex)
        {
            if (restarted)
                *restarted = true;
            return false;
        }
        if (GetModuleHandleA(kLegacyAsiFile))
        {
            other = kLegacyAsiFile;
            return true;
        }
        char name[96]{};
        std::snprintf(name, sizeof(name), "Local\\RedemptionEditor.Script.v1.%lu", static_cast<unsigned long>(GetCurrentProcessId()));
        HANDLE mutex = CreateMutexA(nullptr, FALSE, name);
        if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(mutex);
            other = "another RedemptionEditor.asi";
            return true;
        }
        g_copyMutex = mutex; // kept open: this copy owns the name until it unloads
        return false;
    }

    // The same for the ReShade add-on: the old ReplayEditor.addon64 by its
    // module name, a second new one by the mutex.
    inline bool AnotherAddonLoaded(const wchar_t*& other)
    {
        other = L"";
        if (GetModuleHandleW(kLegacyAddonFileW))
        {
            other = kLegacyAddonFileW;
            return true;
        }
        wchar_t name[96]{};
        swprintf_s(name, L"Local\\RedemptionEditor.Addon.v1.%lu", static_cast<unsigned long>(GetCurrentProcessId()));
        HANDLE mutex = CreateMutexW(nullptr, FALSE, name);
        if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(mutex);
            other = L"another RedemptionEditor.addon64";
            return true;
        }
        g_copyMutex = mutex;
        return false;
    }
}
