// Where the mod writes (src/storage_paths.h): the base folder chosen from what
// each place said (writable, refused, virtualized), the read roots, the
// merged listing of two places, the VirtualStore checks, the file probes, and
// the player's settings file layered over the game folder's
// (mod_identity.h *Layered).
#include "mod_identity.h"
#include "storage_paths.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }

    using namespace replay::storage;

    void TestChoose()
    {
        Choice c = Choose(Access::Writable, Access::Missing, Access::Missing);
        Check(c.place == Place::GameFolder && c.reason == "game_folder_writable", "writable game folder: kept");
        c = Choose(Access::Writable, Access::Denied, Access::Denied);
        Check(c.place == Place::GameFolder, "writable game folder: the fallbacks are not looked at");

        c = Choose(Access::Denied, Access::Writable, Access::Writable);
        Check(c.place == Place::Documents && c.reason == "game_folder_denied", "Program Files: Documents");
        c = Choose(Access::Virtualized, Access::Writable, Access::Missing);
        Check(c.place == Place::Documents && c.reason == "game_folder_virtualized", "a virtualized write is not writable");
        c = Choose(Access::Denied, Access::Denied, Access::Writable);
        Check(c.place == Place::LocalAppData && c.reason == "game_folder_denied+documents_denied", "Documents refused: %LOCALAPPDATA%");
        c = Choose(Access::Denied, Access::Missing, Access::Writable);
        Check(c.place == Place::LocalAppData, "no Documents folder: %LOCALAPPDATA%");
        c = Choose(Access::Denied, Access::Denied, Access::Denied);
        Check(c.place == Place::None && c.reason == "game_folder_denied+documents_denied+localappdata_denied", "nothing takes writes: told");
        Check(std::strcmp(PlaceName(Place::Documents), "documents") == 0 && std::strcmp(AccessName(Access::Virtualized), "virtualized") == 0,
            "names for the log");
    }

    void TestCombine()
    {
        Check(Combine({Access::Writable, Access::Writable}) == Access::Writable, "every check writable");
        Check(Combine({Access::Writable, Access::Denied, Access::Writable}) == Access::Denied, "one data folder refused: refused");
        Check(Combine({Access::Denied, Access::Virtualized}) == Access::Virtualized, "virtualized is told apart");
        Check(Combine({}) == Access::Missing, "no check: missing");
    }

    void TestRoots()
    {
        const std::wstring game = L"C:\\Program Files\\Rockstar Games\\Red Dead Redemption 2";
        const std::wstring docs = L"C:\\Users\\P\\Documents\\Rockstar Games\\Red Dead Redemption 2\\Redemption Editor\\";
        std::vector<std::wstring> roots = ReadRoots(docs, {game, L"", L"c:/users/p/documents/rockstar games/red dead redemption 2/redemption editor"});
        Check(roots.size() == 2, "base and game folder, the base once (case and separators)");
        Check(roots.size() == 2 && roots[0] == docs && roots[1] == game + L"\\", "the base first, every root ending in a backslash");
        roots = ReadRoots(game, {game + L"\\"});
        Check(roots.size() == 1, "a writable game folder is the only root");
        Check(SamePath(L"C:\\A\\b", L"c:/a/B/") && !SamePath(L"C:\\A\\b", L"C:\\A\\bc"), "SamePath");
        Check(WithSlash(L"C:\\x") == L"C:\\x\\" && WithSlash(L"C:\\x\\") == L"C:\\x\\" && WithSlash(L"") == L"", "WithSlash");
    }

    void TestMerge()
    {
        // The base (Documents) and the read only game folder: both listed, a
        // name in both is the base's.
        const std::vector<Entry> base = {{L"005-Wild.rpclip", 0, 50}, {L"004-Valentine.rpclip", 0, 40}};
        const std::vector<Entry> game = {{L"001-Rhodes.rpclip", 0, 10}, {L"004-VALENTINE.rpclip", 0, 5}, {L"002-Wild.rpclip", 0, 20}};
        const std::vector<Entry> merged = Merge({base, game});
        Check(merged.size() == 4, "both places listed, the duplicate once");
        bool baseCopy = false, rhodes = false;
        for (const Entry& e : merged)
        {
            baseCopy = baseCopy || (e.name == L"004-Valentine.rpclip" && e.root == 0 && e.stamp == 40);
            rhodes = rhodes || (e.name == L"001-Rhodes.rpclip" && e.root == 1);
        }
        Check(baseCopy, "a clip in both places opens from the base");
        Check(rhodes, "a game folder clip keeps its root (it opens from there)");
        Check(Merge({{}, game}).size() == 3 && Merge({base, {}}).size() == 2 && Merge({}).empty(), "one empty place");
    }

    void TestVirtualStore()
    {
        Check(VirtualStorePath(L"C:\\Users\\P\\AppData\\Local", L"C:\\Program Files\\Rockstar Games\\RDR2\\x.tmp") ==
                  L"C:\\Users\\P\\AppData\\Local\\VirtualStore\\Program Files\\Rockstar Games\\RDR2\\x.tmp",
            "the VirtualStore copy's path");
        Check(VirtualStorePath(L"C:\\L", L"\\\\server\\share\\x").empty() && VirtualStorePath(L"", L"C:\\x").empty(), "no drive: none");
        Check(LandedElsewhere(L"C:\\Program Files\\G\\t.tmp", L"\\\\?\\C:\\Users\\P\\AppData\\Local\\VirtualStore\\Program Files\\G\\t.tmp"),
            "a final path under the VirtualStore: virtualized");
        Check(!LandedElsewhere(L"C:\\Program Files\\G\\t.tmp", L"\\\\?\\C:\\Program Files\\G\\t.tmp"), "the real path: not virtualized");
        Check(!LandedElsewhere(L"D:\\Games\\RDR2\\t.tmp", L"\\\\?\\F:\\Mounted\\RDR2\\t.tmp"), "a junction or mounted drive: not virtualized");
        Check(!LandedElsewhere(L"C:\\x", L""), "no final path: not virtualized");
    }

    std::wstring Scratch()
    {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        return std::wstring(temp) + L"redemption-storage-tests-" + std::to_wstring(GetCurrentProcessId());
    }

    bool WriteText(const std::wstring& path, const char* text)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD wrote = 0;
        const bool ok = WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &wrote, nullptr) != FALSE;
        CloseHandle(file);
        return ok;
    }

    void TestProbes()
    {
        const std::wstring root = Scratch();
        Check(MakeFolders(root + L"\\a\\b"), "MakeFolders makes every level");
        const ProbeResult writable = ProbeFolder(root + L"\\a\\b");
        Check(writable.access == Access::Writable, "a temp folder is writable");
        WIN32_FIND_DATAW found{};
        HANDLE find = FindFirstFileW((root + L"\\a\\b\\*.tmp").c_str(), &found);
        Check(find == INVALID_HANDLE_VALUE, "the test file is gone");
        if (find != INVALID_HANDLE_VALUE)
            FindClose(find);
        Check(ProbeFolder(root + L"\\missing").access == Access::Missing, "a missing folder");

        const std::wstring locked = root + L"\\RedemptionEditor.ini";
        Check(WriteText(locked, "[RedemptionEditor]\r\n"), "ini written");
        Check(ProbeExistingFile(locked).access == Access::Writable, "a writable ini");
        SetFileAttributesW(locked.c_str(), FILE_ATTRIBUTE_READONLY);
        const ProbeResult refused = ProbeExistingFile(locked);
        Check(refused.access == Access::Denied && refused.error == ERROR_ACCESS_DENIED, "a read only ini: refused");
        SetFileAttributesW(locked.c_str(), FILE_ATTRIBUTE_NORMAL);
        Check(ProbeExistingFile(root + L"\\none.log").access == Access::Writable, "a file not there yet: the folder decides");

        DeleteFileW(locked.c_str());
        RemoveDirectoryW((root + L"\\a\\b").c_str());
        RemoveDirectoryW((root + L"\\a").c_str());
        RemoveDirectoryW(root.c_str());
    }

    // The game folder's ini (shipped) and the player's file in the base folder.
    void TestLayeredIni()
    {
        namespace id = replay::identity;
        const std::wstring root = Scratch() + L"-ini";
        MakeFolders(root + L"\\game");
        MakeFolders(root + L"\\user");
        const std::wstring gameIni = root + L"\\game\\RedemptionEditor.ini";
        const std::wstring userIni = root + L"\\user\\RedemptionEditor.ini";
        Check(WriteText(gameIni,
                  "[RedemptionEditor]\r\nReplayBuffer=1\r\nReplayBufferSeconds=60\r\nLanguage=auto\r\n[Export]\r\nHdr10=0\r\n[Keys]\r\nRecord=F4\r\n"),
            "game ini");
        Check(WriteText(userIni, "[RedemptionEditor]\r\nReplayBufferSeconds=120\r\n[Keys]\r\nRecord=\r\n"), "user ini");
        const std::string game = Ansi(gameIni);

        Check(id::IniIntLayered("ReplayBufferSeconds", 30, game.c_str(), userIni) == 120, "a key the player changed: the user file");
        Check(id::IniIntLayered("ReplayBuffer", 0, game.c_str(), userIni) == 1, "a key he did not: the game folder's");
        Check(id::IniIntLayered("ReplayBufferSeconds", 30, game.c_str(), L"") == 60, "no user file: the game folder's");
        char text[32]{};
        id::IniStringLayered("Language", "x", text, sizeof(text), game.c_str(), userIni);
        Check(std::strcmp(text, "auto") == 0, "a string from the game folder");
        id::ProfileStringLayeredA("Keys", "Record", "\x01", text, sizeof(text), game.c_str(), userIni);
        Check(text[0] == '\0', "[Keys]: an emptied key in the user file wins (unbound)");

        // A write goes to the user file; the game folder's stays as shipped.
        Check(id::IniWriteLayered("ReplayBuffer", "0", game.c_str(), userIni) != FALSE, "write to the user file");
        Check(GetPrivateProfileIntW(L"RedemptionEditor", L"ReplayBuffer", 9, gameIni.c_str()) == 1, "the game folder's ini untouched");
        Check(GetPrivateProfileIntW(L"RedemptionEditor", L"ReplayBuffer", 9, userIni.c_str()) == 0, "the change is in the user file");
        Check(id::IniIntLayered("ReplayBuffer", 1, game.c_str(), userIni) == 0, "and read back from it");

        // The section as logged: the user values over the game folder's.
        char section[1024]{};
        const DWORD length = id::IniSectionLayered(section, sizeof(section), game.c_str(), userIni);
        std::string joined;
        for (DWORD at = 0; at < length && section[at];)
        {
            joined += std::string(section + at) + ";";
            at += static_cast<DWORD>(std::strlen(section + at)) + 1;
        }
        Check(joined == "ReplayBuffer=0;ReplayBufferSeconds=120;Language=auto;", "the merged section");

        // Only the settings file is layered: another file reads plain.
        Check(!id::SettingsFileNameW(root + L"\\game\\other.ini") && id::SettingsFileNameW(gameIni) &&
                  id::SettingsFileNameW(L"C:\\G\\replayprobe.ini"),
            "the settings file by its name");

        DeleteFileW(gameIni.c_str());
        DeleteFileW(userIni.c_str());
        RemoveDirectoryW((root + L"\\game").c_str());
        RemoveDirectoryW((root + L"\\user").c_str());
        RemoveDirectoryW(root.c_str());
    }

    // The tests run from the project root, a writable folder: the game folder
    // is the base, with no user settings file.
    void TestCurrentWritable()
    {
        const Layout layout = Resolve(CurrentFolder());
        Check(layout.place == Place::GameFolder && layout.gameWritable && !layout.failed, "a writable working directory is the base");
        Check(layout.userIni.empty(), "no user settings file then");
        Check(!layout.roots.empty() && SamePath(layout.roots.front(), layout.base), "the base is the first read root");
        Check(SamePath(layout.base, CurrentFolder()), "the base is the working directory");
    }
}

int RunStoragePathsTests()
{
    g_failures = 0;
    TestChoose();
    TestCombine();
    TestRoots();
    TestMerge();
    TestVirtualStore();
    TestProbes();
    TestLayeredIni();
    TestCurrentWritable();
    std::printf("storage paths tests: %s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
    return g_failures;
}
