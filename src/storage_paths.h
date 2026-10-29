#pragma once

// Where the mod keeps what it writes: the clips (ReplayClips), the projects
// (ReplayProjects), the reports (ReplayReports), the developer reports
// (ReplayDebug), RedemptionEditor.log, the crash log, the state files a
// reload leaves and the settings the player changes in the editor.
//
// The game folder when the mod can write there. A game installed under
// C:\Program Files (the Rockstar launcher's default, Steam's
// C:\Program Files (x86)\Steam) takes no writes from a process that is not
// elevated, so the first start checks it: a test file is created, written,
// opened again from a fresh handle and deleted in the game folder and in each
// of its data folders, and RedemptionEditor.log and RedemptionEditor.ini are
// opened for writing when they exist. Any refusal sends every write to
// Documents\Rockstar Games\Red Dead Redemption 2\Redemption Editor (next to
// the game's own saves and the exported videos), or, when Documents takes no
// writes either, to %LOCALAPPDATA%\RedemptionEditor. One base for everything,
// so a clip, its thumbnail and the project that names it never sit apart.
//
// Reads go over every place that may hold data: the base first, then the game
// folder and the other base (a clip recorded before the fallback, or before
// the player changed the folder's permissions, still shows and opens). A file
// in two places is the base's.
//
// UAC virtualization (a write to Program Files that Windows quietly sends to
// %LOCALAPPDATA%\VirtualStore) counts as a refusal: the test file is looked up
// where Windows really put it (the handle's final path, and the VirtualStore
// copy when the process token has virtualization on). Windows never
// virtualizes a 64-bit process such as RDR2.exe; the check guards a launcher
// or a tool that runs the mod's code in a 32-bit process. Nothing is then
// written to the game folder, so no part of the data can end up in the
// VirtualStore.
//
// Header only, used by the ASI (MultiByte), the add-on, the core and the
// tests: the W calls are spelled out. The pure part (Choose, Combine,
// ReadRoots, Merge, VirtualStorePath) takes no file system.

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace replay::storage
{
    inline constexpr wchar_t kClips[] = L"ReplayClips";
    inline constexpr wchar_t kProjects[] = L"ReplayProjects";
    inline constexpr wchar_t kReports[] = L"ReplayReports";
    inline constexpr wchar_t kDebug[] = L"ReplayDebug";
    inline constexpr const wchar_t* kDataFolders[] = {kClips, kProjects, kReports, kDebug};

    // The files of the game folder the mod writes into (checked when present).
    inline constexpr const wchar_t* kWrittenFiles[] = {L"RedemptionEditor.log", L"RedemptionEditor.ini"};

    // Under Documents, and under %LOCALAPPDATA% (where the crash dumps already go).
    inline constexpr wchar_t kDocumentsTail[] = L"Rockstar Games\\Red Dead Redemption 2\\Redemption Editor";
    inline constexpr wchar_t kLocalTail[] = L"RedemptionEditor";

    enum class Access : std::uint8_t
    {
        Writable,    // created, written, seen at its real path, deleted
        Denied,      // a step refused (access denied, read only, no delete)
        Virtualized, // the write went to the VirtualStore
        Missing,     // the folder is not there and could not be made
    };

    inline const char* AccessName(Access access)
    {
        switch (access)
        {
        case Access::Writable: return "writable";
        case Access::Denied: return "denied";
        case Access::Virtualized: return "virtualized";
        case Access::Missing: return "missing";
        }
        return "?";
    }

    enum class Place : std::uint8_t
    {
        GameFolder,
        Documents,
        LocalAppData,
        None, // nothing takes writes: the game folder is kept, and the player told
    };

    inline const char* PlaceName(Place place)
    {
        switch (place)
        {
        case Place::GameFolder: return "game_folder";
        case Place::Documents: return "documents";
        case Place::LocalAppData: return "localappdata";
        case Place::None: return "none";
        }
        return "?";
    }

    // The game folder's verdict from its checks (the folder, each data folder,
    // each written file): one refusal is enough. Virtualized wins over Denied
    // (it is the one to tell apart), an empty list is Missing.
    inline Access Combine(const std::vector<Access>& checks)
    {
        if (checks.empty())
            return Access::Missing;
        bool denied = false, missing = false;
        for (const Access access : checks)
        {
            if (access == Access::Virtualized)
                return Access::Virtualized;
            denied = denied || access == Access::Denied;
            missing = missing || access == Access::Missing;
        }
        return denied ? Access::Denied : missing ? Access::Missing : Access::Writable;
    }

    struct Choice
    {
        Place place = Place::None;
        // For the STORAGE_BASE log line: "game_folder_writable", else what each
        // place said ("game_folder_denied+documents_writable").
        std::string reason;
    };

    // The game folder when it takes writes, else Documents, else
    // %LOCALAPPDATA%. `documents` and `local` are only looked at (and only
    // need checking) when the places before them refused.
    inline Choice Choose(Access game, Access documents, Access local)
    {
        Choice choice;
        if (game == Access::Writable)
        {
            choice.place = Place::GameFolder;
            choice.reason = "game_folder_writable";
            return choice;
        }
        choice.reason = std::string("game_folder_") + AccessName(game);
        if (documents == Access::Writable)
        {
            choice.place = Place::Documents;
            return choice;
        }
        choice.reason += std::string("+documents_") + AccessName(documents);
        if (local == Access::Writable)
        {
            choice.place = Place::LocalAppData;
            return choice;
        }
        choice.reason += std::string("+localappdata_") + AccessName(local);
        choice.place = Place::None;
        return choice;
    }

    // ---- Paths (pure) ----

    inline std::wstring WithSlash(std::wstring folder)
    {
        if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/')
            folder += L'\\';
        return folder;
    }

    inline wchar_t FoldCase(wchar_t c)
    {
        if (c == L'/')
            return L'\\';
        return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
    }

    // Windows names: case and the trailing separator do not matter (ASCII case
    // folding: enough for paths the mod built from the same sources).
    inline bool SamePath(const std::wstring& a, const std::wstring& b)
    {
        const std::wstring x = WithSlash(a), y = WithSlash(b);
        if (x.size() != y.size())
            return false;
        for (std::size_t n = 0; n < x.size(); ++n)
            if (FoldCase(x[n]) != FoldCase(y[n]))
                return false;
        return true;
    }

    // The folders reads go over: the base, then each other place, once each
    // (empty ones dropped), every one ending in '\'.
    inline std::vector<std::wstring> ReadRoots(const std::wstring& base, const std::vector<std::wstring>& others)
    {
        std::vector<std::wstring> roots;
        const auto add = [&roots](const std::wstring& folder) {
            if (folder.empty())
                return;
            for (const std::wstring& root : roots)
                if (SamePath(root, folder))
                    return;
            roots.push_back(WithSlash(folder));
        };
        add(base);
        for (const std::wstring& other : others)
            add(other);
        return roots;
    }

    struct Entry
    {
        std::wstring name;        // file name, no folder
        std::size_t root = 0;     // index into the read roots
        std::uint64_t stamp = 0;  // last write time (FILETIME)
    };

    // One listing out of the roots' (perRoot[n] holds root n's files): a name
    // found in two roots is the first root's (the base); order kept, root by
    // root.
    inline std::vector<Entry> Merge(const std::vector<std::vector<Entry>>& perRoot)
    {
        std::vector<Entry> out;
        for (std::size_t root = 0; root < perRoot.size(); ++root)
        {
            for (const Entry& entry : perRoot[root])
            {
                bool seen = false;
                for (const Entry& kept : out)
                    if (SamePath(kept.name, entry.name))
                    {
                        seen = true;
                        break;
                    }
                if (seen)
                    continue;
                out.push_back(entry);
                out.back().root = root;
            }
        }
        return out;
    }

    // Where UAC virtualization puts a write to `path`:
    // %LOCALAPPDATA%\VirtualStore\<path without its drive>. Empty for a path
    // with no drive letter.
    inline std::wstring VirtualStorePath(const std::wstring& localAppData, const std::wstring& path)
    {
        if (localAppData.empty() || path.size() < 3 || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/'))
            return {};
        return WithSlash(localAppData) + L"VirtualStore\\" + path.substr(3);
    }

    // A final path (GetFinalPathNameByHandle: "\\?\C:\..." or
    // "\\?\UNC\server\...") that is not the one written: the write went
    // somewhere else (the VirtualStore). Only the "\\?\" prefix is dropped.
    inline bool LandedElsewhere(const std::wstring& written, std::wstring finalPath)
    {
        if (finalPath.rfind(L"\\\\?\\UNC\\", 0) == 0)
            finalPath = L"\\\\" + finalPath.substr(8);
        else if (finalPath.rfind(L"\\\\?\\", 0) == 0)
            finalPath = finalPath.substr(4);
        if (finalPath.empty())
            return false;
        // Under the VirtualStore is always elsewhere; otherwise compare (a
        // drive mapped to a folder or a junction also changes the final
        // path, so only a VirtualStore one counts).
        std::wstring lower;
        for (const wchar_t c : finalPath)
            lower += FoldCase(c);
        return lower.find(L"\\virtualstore\\") != std::wstring::npos && !SamePath(written, finalPath);
    }

    // ---- The file system ----

    inline std::string Utf8(const std::wstring& text)
    {
        if (text.empty())
            return {};
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
        if (size > 0)
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
        return out;
    }

    // In the ANSI code page, for the A calls (fopen, the profile A calls).
    inline std::string Ansi(const std::wstring& text)
    {
        if (text.empty())
            return {};
        const int size = WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
        if (size > 0)
            WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
        return out;
    }

    inline std::wstring Widen(const std::string& ansi)
    {
        if (ansi.empty())
            return {};
        const int size = MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), static_cast<int>(ansi.size()), nullptr, 0);
        std::wstring out(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
        if (size > 0)
            MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), static_cast<int>(ansi.size()), out.data(), size);
        return out;
    }

    inline bool IsFolder(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    inline bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

    // Every missing folder of `folder` made (CreateDirectory one level at a
    // time; no shell call, so it is safe while the loader lock is held).
    inline bool MakeFolders(const std::wstring& folder)
    {
        if (folder.empty())
            return false;
        if (IsFolder(folder))
            return true;
        std::wstring path = folder;
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
            path.pop_back();
        const std::size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash > 2)
            MakeFolders(path.substr(0, slash));
        return CreateDirectoryW(path.c_str(), nullptr) != FALSE || IsFolder(path);
    }

    inline std::wstring EnvironmentFolder(const wchar_t* name)
    {
        wchar_t value[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(name, value, MAX_PATH);
        return length == 0 || length >= MAX_PATH ? std::wstring() : std::wstring(value, length);
    }

    // The Documents folder as Explorer has it (moved, or redirected to
    // OneDrive): the User Shell Folders "Personal" value, as
    // SHGetKnownFolderPath(FOLDERID_Documents) reads it (the registry, no
    // shell call: safe under the loader lock), else %USERPROFILE%\Documents.
    inline std::wstring DocumentsFolder()
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", 0,
                KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
        {
            wchar_t raw[MAX_PATH * 2]{};
            DWORD bytes = sizeof(raw) - sizeof(wchar_t);
            DWORD type = 0;
            const LSTATUS read = RegQueryValueExW(key, L"Personal", nullptr, &type, reinterpret_cast<BYTE*>(raw), &bytes);
            RegCloseKey(key);
            if (read == ERROR_SUCCESS && (type == REG_EXPAND_SZ || type == REG_SZ) && raw[0])
            {
                wchar_t expanded[MAX_PATH * 2]{};
                const DWORD length = ExpandEnvironmentStringsW(raw, expanded, MAX_PATH * 2);
                if (length > 0 && length <= MAX_PATH)
                    return expanded;
            }
        }
        const std::wstring profile = EnvironmentFolder(L"USERPROFILE");
        return profile.empty() ? std::wstring() : profile + L"\\Documents";
    }

    inline std::wstring LocalAppDataFolder() { return EnvironmentFolder(L"LOCALAPPDATA"); }

    // UAC virtualization on for this process (a 32-bit process without a
    // manifest, on an account that is not elevated). Always off for RDR2.exe.
    inline bool TokenVirtualized()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
        DWORD enabled = 0, size = 0;
        const bool on = GetTokenInformation(token, TokenVirtualizationEnabled, &enabled, sizeof(enabled), &size) && enabled != 0;
        CloseHandle(token);
        return on;
    }

    struct ProbeResult
    {
        Access access = Access::Missing;
        DWORD error = 0;          // the refusal's GetLastError
        const char* step = "";    // which step refused ("create", "write", "reopen", "delete", "open")
    };

    // The test file: created, written, its final path checked, opened again
    // from a fresh handle (it must be there, at the real path), deleted.
    inline ProbeResult ProbeFolder(const std::wstring& folder)
    {
        ProbeResult result;
        if (!IsFolder(folder))
        {
            result.error = GetLastError();
            result.step = "folder";
            return result;
        }
        static volatile LONG counter = 0;
        wchar_t name[96]{};
        swprintf_s(name, L".redemption-editor-write-test-%lu-%ld.tmp", GetCurrentProcessId(), InterlockedIncrement(&counter));
        const std::wstring path = WithSlash(folder) + name;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            result.error = GetLastError();
            result.access = Access::Denied;
            result.step = "create";
            return result;
        }
        static const char kBytes[] = "RE";
        DWORD wrote = 0;
        const bool written = WriteFile(file, kBytes, 2, &wrote, nullptr) && wrote == 2;
        const DWORD writeError = written ? 0 : GetLastError();
        wchar_t finalPath[MAX_PATH * 2]{};
        const DWORD finalLength = GetFinalPathNameByHandleW(file, finalPath, MAX_PATH * 2, FILE_NAME_NORMALIZED);
        CloseHandle(file);
        bool virtualized = finalLength > 0 && finalLength < MAX_PATH * 2 && LandedElsewhere(path, finalPath);
        if (!virtualized && TokenVirtualized())
        {
            const std::wstring copy = VirtualStorePath(LocalAppDataFolder(), path);
            virtualized = !copy.empty() && Exists(copy);
        }
        // A fresh handle at the real path finds it (no share mode needed: ours is closed).
        HANDLE again = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD reopenError = again == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        if (again != INVALID_HANDLE_VALUE)
            CloseHandle(again);
        const bool deleted = DeleteFileW(path.c_str()) != FALSE;
        const DWORD deleteError = deleted ? 0 : GetLastError();
        if (virtualized)
        {
            result.access = Access::Virtualized;
            result.step = "virtualstore";
        }
        else if (!written)
        {
            result.access = Access::Denied;
            result.error = writeError;
            result.step = "write";
        }
        else if (again == INVALID_HANDLE_VALUE)
        {
            result.access = Access::Denied;
            result.error = reopenError;
            result.step = "reopen";
        }
        else if (!deleted)
        {
            result.access = Access::Denied;
            result.error = deleteError;
            result.step = "delete";
        }
        else
            result.access = Access::Writable;
        return result;
    }

    // An existing file the mod rewrites (the log, the ini): opened for writing
    // and closed untouched. Missing is fine (the folder check covers making it);
    // a sharing violation is another process holding it, not a refusal.
    inline ProbeResult ProbeExistingFile(const std::wstring& path)
    {
        ProbeResult result;
        result.access = Access::Writable;
        if (!Exists(path))
            return result;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
            return result;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
            return result;
        result.access = Access::Denied;
        result.error = error;
        result.step = "open";
        return result;
    }

    struct Layout
    {
        std::wstring game;      // the game folder (the working directory), ending in '\'
        std::wstring base;      // where every write goes, ending in '\'
        std::wstring documents; // the Documents candidate (empty when unknown)
        std::wstring local;     // the %LOCALAPPDATA% candidate (empty when unknown)
        std::vector<std::wstring> roots; // where reads look, the base first
        Place place = Place::GameFolder;
        bool gameWritable = true;
        bool failed = false; // no place takes writes (Place::None)
        std::string reason;
        // The first refusal in the game folder, and the fallback's when it
        // failed too, for the log and the notice: "C:\...\ReplayClips: create
        // error 5".
        std::string gameDetail;
        std::string fallbackDetail;
        // RedemptionEditor.ini of the base when it is not the game folder: the
        // player's changes, read over the game folder's.
        std::wstring userIni;
    };

    inline std::string Detail(const std::wstring& where, const ProbeResult& probe)
    {
        return Utf8(where) + ": " + AccessName(probe.access) + " (" + probe.step + " error " + std::to_string(probe.error) + ")";
    }

    // The base folder for `game` (checked as the header says), the fallbacks
    // made and checked only when needed.
    inline Layout Resolve(const std::wstring& gameFolder)
    {
        Layout layout;
        layout.game = WithSlash(gameFolder);
        layout.documents = [] {
            const std::wstring documents = DocumentsFolder();
            return documents.empty() ? std::wstring() : WithSlash(WithSlash(documents) + kDocumentsTail);
        }();
        layout.local = [] {
            const std::wstring local = LocalAppDataFolder();
            return local.empty() ? std::wstring() : WithSlash(WithSlash(local) + kLocalTail);
        }();

        std::vector<Access> checks;
        const auto check = [&](const std::wstring& where, const ProbeResult& probe) {
            checks.push_back(probe.access);
            if (probe.access != Access::Writable && layout.gameDetail.empty())
                layout.gameDetail = Detail(where, probe);
        };
        check(layout.game, ProbeFolder(layout.game));
        if (checks.back() == Access::Writable)
        {
            for (const wchar_t* folder : kDataFolders)
            {
                const std::wstring path = layout.game + folder;
                if (IsFolder(path))
                    check(path, ProbeFolder(path));
            }
            for (const wchar_t* file : kWrittenFiles)
                check(layout.game + file, ProbeExistingFile(layout.game + file));
        }
        const Access game = Combine(checks);

        Access documents = Access::Missing, local = Access::Missing;
        ProbeResult documentsProbe, localProbe;
        if (game != Access::Writable)
        {
            if (!layout.documents.empty() && MakeFolders(layout.documents))
                documents = (documentsProbe = ProbeFolder(layout.documents)).access;
            if (documents != Access::Writable && !layout.local.empty() && MakeFolders(layout.local))
                local = (localProbe = ProbeFolder(layout.local)).access;
        }
        const Choice choice = Choose(game, documents, local);
        layout.place = choice.place;
        layout.reason = choice.reason;
        layout.gameWritable = game == Access::Writable;
        layout.failed = choice.place == Place::None;
        switch (choice.place)
        {
        case Place::Documents: layout.base = layout.documents; break;
        case Place::LocalAppData: layout.base = layout.local; break;
        default: layout.base = layout.game; break; // the game folder, or nothing better
        }
        if (layout.failed)
            layout.fallbackDetail = Detail(layout.documents.empty() ? std::wstring(L"Documents") : layout.documents, documentsProbe) +
                "; " + Detail(layout.local.empty() ? std::wstring(L"%LOCALAPPDATA%") : layout.local, localProbe);
        if (!SamePath(layout.base, layout.game))
            layout.userIni = layout.base + L"RedemptionEditor.ini";
        // Reads: the base, the game folder, then the other fallback when it
        // was made by an earlier start (no folder is made for reading).
        std::vector<std::wstring> others{layout.game};
        for (const std::wstring& other : {layout.documents, layout.local})
            if (!other.empty() && IsFolder(other))
                others.push_back(other);
        layout.roots = ReadRoots(layout.base, others);
        return layout;
    }

    inline std::wstring CurrentFolder()
    {
        wchar_t folder[MAX_PATH]{};
        const DWORD length = GetCurrentDirectoryW(MAX_PATH, folder);
        return length == 0 || length >= MAX_PATH ? std::wstring(L".") : std::wstring(folder, length);
    }

    // Decided once per module, at its first use (the ASI, the core and the
    // loader each decide; the checks give each the same answer).
    inline const Layout& Current()
    {
        static const Layout layout = Resolve(CurrentFolder());
        return layout;
    }

    // ---- What the mod calls ----

    // A file or folder of the base ("RedemptionEditor.log", "ReplayClips\\x.rpclip").
    inline std::wstring Path(const std::wstring& relative) { return Current().base + relative; }

    // A data folder of the base, made when missing (no trailing '\').
    inline std::wstring Folder(const wchar_t* name)
    {
        const std::wstring folder = Current().base + name;
        MakeFolders(folder);
        return folder;
    }

    // Where a file to read is: the first root that has it, else the base's
    // path (where a new one would be written).
    inline std::wstring Find(const std::wstring& relative)
    {
        for (const std::wstring& root : Current().roots)
            if (Exists(root + relative))
                return root + relative;
        return Path(relative);
    }

    // The roots that have the data folder `name` (no trailing '\'), base first.
    inline std::vector<std::wstring> FolderRoots(const wchar_t* name)
    {
        std::vector<std::wstring> out;
        for (const std::wstring& root : Current().roots)
            if (IsFolder(root + name))
                out.push_back(root + name);
        return out;
    }

    // The files of `name` matching `pattern` ("*.rpclip") in every root, a
    // name in two roots once (the base's). `folders` set: the root folder of
    // each entry, by Entry::root. Directories are left out unless asked.
    inline std::vector<Entry> List(const wchar_t* name, const wchar_t* pattern, std::vector<std::wstring>* folders = nullptr,
        bool directories = false)
    {
        std::vector<std::vector<Entry>> perRoot;
        std::vector<std::wstring> rootFolders;
        for (const std::wstring& root : Current().roots)
        {
            const std::wstring folder = root + name;
            rootFolders.push_back(folder);
            perRoot.emplace_back();
            WIN32_FIND_DATAW found{};
            HANDLE find = FindFirstFileW((folder + L"\\" + pattern).c_str(), &found);
            if (find == INVALID_HANDLE_VALUE)
                continue;
            do
            {
                const bool directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (directory != directories || std::wstring(found.cFileName) == L"." || std::wstring(found.cFileName) == L"..")
                    continue;
                Entry entry;
                entry.name = found.cFileName;
                entry.stamp = (static_cast<std::uint64_t>(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
                perRoot.back().push_back(entry);
            } while (FindNextFileW(find, &found));
            FindClose(find);
        }
        if (folders)
            *folders = rootFolders;
        return Merge(perRoot);
    }

    // The base path of a file another root holds (same folder and name under
    // the base): a project or a clip opened from the read only game folder
    // is saved there. A path already in the base is returned as it is.
    inline std::wstring WritablePath(const std::wstring& path)
    {
        const Layout& layout = Current();
        const auto under = [&path](const std::wstring& root) {
            if (path.size() <= root.size())
                return false;
            return SamePath(path.substr(0, root.size()), root);
        };
        if (under(layout.base))
            return path;
        for (const std::wstring& root : layout.roots)
            if (under(root))
                return layout.base + path.substr(root.size());
        return path;
    }

    // STORAGE_BASE,base=...,game_folder_writable=0|1,reason=... (and the
    // refusal seen, for the log).
    inline std::string LogLine()
    {
        const Layout& layout = Current();
        std::string line = "STORAGE_BASE,base=" + Utf8(layout.base) + ",game_folder_writable=" + (layout.gameWritable ? "1" : "0") +
            ",reason=" + layout.reason + ",place=" + PlaceName(layout.place) + ",failed=" + (layout.failed ? "1" : "0");
        line += ",roots=";
        for (std::size_t n = 0; n < layout.roots.size(); ++n)
            line += (n ? "|" : "") + Utf8(layout.roots[n]);
        if (!layout.gameDetail.empty())
            line += ",game_refusal=" + layout.gameDetail;
        if (!layout.fallbackDetail.empty())
            line += ",fallback_refusal=" + layout.fallbackDetail;
        return line;
    }
}
