// QA kit: the QA report's extension and zip.
// See qa_bundle.h. Report thread only; Win32 only.

#include "qa_bundle.h"

#include "clip_compress.h"
#include "qa_marks.h"
#include "qa_report.h" // the marks line of report.txt
#include "qa_zip.h"

#include <Windows.h>
#include <psapi.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>

#pragma comment(lib, "version.lib")

namespace replay::qa::bundle
{
    namespace
    {
        std::wstring Widen(const std::string& text)
        {
            if (text.empty())
                return {};
            const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
            return out;
        }

        std::string Narrow(const std::wstring& text)
        {
            if (text.empty())
                return {};
            const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
            return out;
        }

        bool Cancelled(const Input& input) { return input.cancel && input.cancel->load(); }

        std::wstring GameFolder()
        {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring folder(path);
            return folder.substr(0, folder.find_last_of(L'\\') + 1);
        }

        std::string FileVersion(const std::wstring& path)
        {
            DWORD handle = 0;
            const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
            if (!size)
                return "none";
            std::vector<std::uint8_t> data(size);
            if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
                return "none";
            VS_FIXEDFILEINFO* info = nullptr;
            UINT length = 0;
            if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info)
                return "none";
            char text[48];
            std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
            return text;
        }

        HANDLE OpenShared(const std::wstring& path)
        {
            return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        }

        std::string Sha256(const std::wstring& path, std::uint64_t& bytes)
        {
            bytes = 0;
            HANDLE file = OpenShared(path);
            if (file == INVALID_HANDLE_VALUE)
                return "missing";
            std::string text = "unavailable";
            HCRYPTPROV provider = 0;
            HCRYPTHASH hash = 0;
            if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
                CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash))
            {
                std::vector<BYTE> buffer(1 << 20);
                DWORD read = 0;
                bool ok = true;
                while (ok && ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0)
                {
                    ok = CryptHashData(hash, buffer.data(), read, 0) != FALSE;
                    bytes += read;
                }
                BYTE digest[32]{};
                DWORD digestSize = sizeof(digest);
                if (ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0) && digestSize == sizeof(digest))
                {
                    char hex[65]{};
                    for (DWORD n = 0; n < digestSize; ++n)
                        std::snprintf(hex + n * 2, 3, "%02x", digest[n]);
                    text = hex;
                }
            }
            if (hash)
                CryptDestroyHash(hash);
            if (provider)
                CryptReleaseContext(provider, 0);
            CloseHandle(file);
            return text;
        }

        bool CopyShared(const std::wstring& from, const std::wstring& to, const Input& input, std::uint64_t& bytes)
        {
            bytes = 0;
            HANDLE source = OpenShared(from);
            if (source == INVALID_HANDLE_VALUE)
                return false;
            HANDLE target = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (target == INVALID_HANDLE_VALUE)
            {
                CloseHandle(source);
                return false;
            }
            std::vector<char> buffer(1 << 20);
            DWORD read = 0;
            bool ok = true;
            while (ok && ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0)
            {
                DWORD wrote = 0;
                ok = WriteFile(target, buffer.data(), read, &wrote, nullptr) && wrote == read && !Cancelled(input);
                bytes += read;
            }
            CloseHandle(target);
            CloseHandle(source);
            if (!ok)
                DeleteFileW(to.c_str());
            return ok;
        }

        bool ReadAll(const std::wstring& path, std::vector<std::uint8_t>& out, std::uint64_t limit)
        {
            out.clear();
            HANDLE file = OpenShared(path);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            LARGE_INTEGER size{};
            bool ok = GetFileSizeEx(file, &size) && static_cast<std::uint64_t>(size.QuadPart) <= limit;
            if (ok)
            {
                out.resize(static_cast<std::size_t>(size.QuadPart));
                std::size_t done = 0;
                while (ok && done < out.size())
                {
                    DWORD read = 0;
                    const DWORD want = static_cast<DWORD>((std::min<std::size_t>)(out.size() - done, 1u << 24));
                    ok = ReadFile(file, out.data() + done, want, &read, nullptr) && read > 0;
                    done += read;
                }
            }
            CloseHandle(file);
            if (!ok)
                out.clear();
            return ok;
        }

        bool WriteAll(const std::wstring& path, const void* data, std::size_t size)
        {
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            bool ok = true;
            std::size_t done = 0;
            while (ok && done < size)
            {
                DWORD wrote = 0;
                const DWORD want = static_cast<DWORD>((std::min<std::size_t>)(size - done, 1u << 24));
                ok = WriteFile(file, bytes + done, want, &wrote, nullptr) && wrote == want;
                done += wrote;
            }
            CloseHandle(file);
            return ok;
        }

        bool WriteText(const std::wstring& path, const std::string& text) { return WriteAll(path, text.data(), text.size()); }

        std::vector<WIN32_FIND_DATAW> List(const std::wstring& pattern)
        {
            std::vector<WIN32_FIND_DATAW> out;
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW(pattern.c_str(), &data);
            if (find == INVALID_HANDLE_VALUE)
                return out;
            do
            {
                if (std::wcscmp(data.cFileName, L".") != 0 && std::wcscmp(data.cFileName, L"..") != 0)
                    out.push_back(data);
            } while (FindNextFileW(find, &data));
            FindClose(find);
            return out;
        }

        std::uint64_t Size(const WIN32_FIND_DATAW& data)
        {
            return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        }

        std::string LocalTimeText(const FILETIME& time)
        {
            FILETIME local{};
            SYSTEMTIME st{};
            FileTimeToLocalFileTime(&time, &local);
            FileTimeToSystemTime(&local, &st);
            char text[32];
            std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
            return text;
        }

        std::wstring RegString(HKEY root, const std::wstring& key, const wchar_t* value)
        {
            wchar_t text[512]{};
            DWORD size = sizeof(text);
            if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ, nullptr, text, &size) != ERROR_SUCCESS)
                return {};
            return text;
        }

        std::string WindowsVersion()
        {
            using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
            OSVERSIONINFOW info{};
            info.dwOSVersionInfoSize = sizeof(info);
            const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            const auto get = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
            if (!get || get(&info) != 0)
                return "unknown";
            const std::wstring key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
            const std::string display = Narrow(RegString(HKEY_LOCAL_MACHINE, key, L"DisplayVersion"));
            const std::string product = Narrow(RegString(HKEY_LOCAL_MACHINE, key, L"ProductName"));
            DWORD ubr = 0, size = sizeof(ubr);
            RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
            char text[200];
            // Windows 11 still says "Windows 10" in ProductName: the build tells (22000 and later is 11).
            std::snprintf(text, sizeof(text), "%lu.%lu.%lu.%lu (%s%s, %s)", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber,
                ubr, info.dwBuildNumber >= 22000 ? "Windows 11; registry says " : "", product.empty() ? "?" : product.c_str(),
                display.empty() ? "?" : display.c_str());
            return text;
        }

        std::string GpuLines()
        {
            // The display adapters' driver keys (the class GUID of display
            // adapters): name, provider, driver version and date.
            const std::wstring base = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\";
            std::string out;
            for (int index = 0; index < 16; ++index)
            {
                wchar_t sub[8];
                std::swprintf(sub, 8, L"%04d", index);
                const std::wstring key = base + sub;
                const std::string name = Narrow(RegString(HKEY_LOCAL_MACHINE, key, L"DriverDesc"));
                if (name.empty())
                    continue;
                const std::string version = Narrow(RegString(HKEY_LOCAL_MACHINE, key, L"DriverVersion"));
                const std::string provider = Narrow(RegString(HKEY_LOCAL_MACHINE, key, L"ProviderName"));
                const std::string date = Narrow(RegString(HKEY_LOCAL_MACHINE, key, L"DriverDate"));
                std::string vendor;
                if (provider.find("NVIDIA") != std::string::npos)
                {
                    const std::string nv = NvidiaDriverVersion(version);
                    if (!nv.empty())
                        vendor = " (NVIDIA " + nv + ")";
                }
                out += "gpu: " + name + ", driver " + (version.empty() ? "?" : version) + vendor + ", " +
                    (provider.empty() ? "?" : provider) + ", " + (date.empty() ? "?" : date) + "\r\n";
            }
            return out.empty() ? "gpu: unknown\r\n" : out;
        }

        // Every module loaded in the game that exports ReShade's add-on
        // entry (the ReShade DLL, whatever its file name: dxgi.dll, a
        // Vulkan layer's ReShade64.dll).
        std::string ReShadeLines()
        {
            HMODULE modules[1024];
            DWORD needed = 0;
            if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
                return "reshade: unknown (module list failed)\r\n";
            std::string out;
            const DWORD count = (std::min<DWORD>)(needed / sizeof(HMODULE), 1024);
            for (DWORD n = 0; n < count; ++n)
            {
                if (!GetProcAddress(modules[n], "ReShadeRegisterAddon"))
                    continue;
                wchar_t path[MAX_PATH]{};
                GetModuleFileNameW(modules[n], path, MAX_PATH);
                out += "reshade: " + FileVersion(path) + ", " + Narrow(path) + "\r\n";
            }
            return out.empty() ? "reshade: not loaded (the editor needs it)\r\n" : out;
        }

        std::wstring LowerW(std::wstring text)
        {
            for (wchar_t& c : text)
                c = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
            return text;
        }

        std::string FileLine(const char* label, const std::wstring& path)
        {
            std::uint64_t bytes = 0;
            const std::string sha = Sha256(path, bytes);
            return std::string(label) + ": " + Narrow(path) + ", version " + FileVersion(path) + ", " + std::to_string(bytes) +
                " bytes, sha256 " + sha + "\r\n";
        }

        std::string SysInfo(const Input& input, Result& result)
        {
            const std::wstring game = GameFolder();
            std::string text = "Redemption Editor QA report: system\r\n";
            text += "mod_version: " + input.asiVersion + "\r\n";
            text += "windows: " + WindowsVersion() + "\r\n";
            std::string cpu = Narrow(RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString"));
            while (!cpu.empty() && cpu.back() == ' ')
                cpu.pop_back();
            SYSTEM_INFO system{};
            GetNativeSystemInfo(&system);
            text += "cpu: " + (cpu.empty() ? std::string("?") : cpu) + ", " + std::to_string(system.dwNumberOfProcessors) + " threads\r\n";
            MEMORYSTATUSEX memory{};
            memory.dwLength = sizeof(memory);
            if (GlobalMemoryStatusEx(&memory))
                text += "ram: " + MegabytesText(memory.ullTotalPhys) + " MB total, " + MegabytesText(memory.ullAvailPhys) +
                    " MB free, load " + std::to_string(memory.dwMemoryLoad) + "%, commit " + MegabytesText(memory.ullTotalPageFile) +
                    " MB\r\n";
            text += GpuLines();
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            result.gameExeVersion = FileVersion(exe);
            result.gameBuildMatches = GameBuildMatches(result.gameExeVersion);
            text += "game_exe_build_match: " + std::string(result.gameBuildMatches == 1 ? "yes" : result.gameBuildMatches == 0 ? "NO" : "unknown") +
                " (the mod's addresses are from build " + std::to_string(kGameBuildMajor) + "." + std::to_string(kGameBuildMinor) + ")\r\n";
            text += "scripthook_game_version: " + std::to_string(input.scriptHookGameVersion) + "\r\n";
            text += FileLine("game_exe", exe);
            text += FileLine("scripthook", game + L"ScriptHookRDR2.dll");
            text += ReShadeLines();
            text += "asi: " + Narrow(input.asiPath) + ", " + input.asiVersion + ", sha256 " + input.asiSha256 + "\r\n";
            text += FileLine("editor_core", game + L"RedemptionEditorCore.dll");
            text += FileLine("editor_addon", game + L"RedemptionEditor.addon64");
            text += FileLine("zlib", game + L"RedemptionEditor\\dlls\\z.dll");
            text += "env_mods: " + EnvModsLine() + "\r\n"; // what is loaded, not only present
            text += "mods in the game folder (.asi, .addon64, non-game .dll):\r\n";
            for (const std::wstring& folder : {game, game + L"scripts\\"})
            {
                for (const WIN32_FIND_DATAW& data : List(folder + L"*"))
                {
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        continue;
                    const std::string name = Narrow(data.cFileName);
                    if (!ListedModFile(name))
                        continue;
                    text += "  " + Narrow(folder.substr(game.size())) + name + ", " + std::to_string(Size(data)) + " bytes, " +
                        LocalTimeText(data.ftLastWriteTime) + ", version " + FileVersion(folder + data.cFileName) + "\r\n";
                }
            }
            return text;
        }

        // The "clip" value of a mark.json ("" none).
        std::string ClipOfMarkJson(const std::string& json)
        {
            const std::string key = "\"clip\": \"";
            const std::size_t at = json.find(key);
            if (at == std::string::npos)
                return {};
            const std::size_t from = at + key.size();
            const std::size_t end = json.find('"', from);
            return end == std::string::npos ? std::string() : json.substr(from, end - from);
        }

        // Where a data file to read is: the first read root that has it (the
        // working directory when none are given), else the first's path.
        std::wstring FindData(const Input& input, const std::wstring& relative)
        {
            if (input.readRoots.empty())
                return relative;
            for (const std::wstring& root : input.readRoots)
                if (GetFileAttributesW((root + relative).c_str()) != INVALID_FILE_ATTRIBUTES)
                    return root + relative;
            return input.readRoots.front() + relative;
        }

        void MoveMarks(const Input& input, Result& result, std::vector<std::string>& clips)
        {
            const std::wstring marks = input.reportFolder + L"\\marks";
            std::string all = "[\n";
            bool first = true;
            for (const WIN32_FIND_DATAW& data : List(input.dataRoot + L"ReplayReports\\marks\\mark-*"))
            {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                CreateDirectoryW(marks.c_str(), nullptr);
                const std::wstring from = input.dataRoot + L"ReplayReports\\marks\\" + data.cFileName;
                const std::wstring to = marks + L"\\" + data.cFileName;
                if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED))
                {
                    if (result.error.empty())
                        result.error = "mark_move_" + std::to_string(GetLastError());
                    continue;
                }
                ++result.marks;
                result.markFolders.push_back(Narrow(data.cFileName));
                std::vector<std::uint8_t> json;
                if (ReadAll(to + L"\\mark.json", json, 16u << 20))
                {
                    const std::string text(json.begin(), json.end());
                    all += (first ? "" : ",\n") + text;
                    first = false;
                    const std::string clip = ClipOfMarkJson(text);
                    if (!clip.empty() && std::find(clips.begin(), clips.end(), clip) == clips.end())
                        clips.push_back(clip);
                }
            }
            all += "\n]\n";
            if (result.marks)
                WriteText(input.reportFolder + L"\\marks.json", all);
        }

        void CopyClips(const Input& input, Result& result, const std::vector<std::string>& clips)
        {
            const std::wstring target = input.reportFolder + L"\\clips";
            for (const std::string& clip : clips)
            {
                if (Cancelled(input))
                    return;
                if (clip.find('\\') != std::string::npos || clip.find('/') != std::string::npos || clip.find("..") != std::string::npos)
                    continue;
                const std::wstring from = FindData(input, L"ReplayClips\\" + Widen(clip));
                if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                    result.missingClips.push_back(clip);
                    continue;
                }
                CreateDirectoryW(target.c_str(), nullptr);
                const std::wstring to = target + L"\\" + Widen(clip);
                std::vector<std::uint8_t> data;
                // A plain clip is compressed (the mod's zstd format, a .rpclip
                // the editor opens and the zstd tool reads); a compressed one
                // (zstd or the packed container), or one too big for memory,
                // is copied as it is.
                if (ReadAll(from, data, clipcompress::kMaxClipBytes) && clipcompress::IsPlainClip(data.data(), data.size()))
                {
                    std::vector<std::uint8_t> packed;
                    std::string error;
                    if (clipcompress::Compress(data.data(), data.size(), input.clipCompressLevel, input.cancel, packed, error))
                    {
                        std::vector<std::uint8_t> summary;
                        if (clipcompress::Summary(data.data(), data.size(), summary))
                            clipcompress::AppendSummaryFrame(summary, packed);
                        if (WriteAll(to, packed.data(), packed.size()))
                        {
                            ++result.clips;
                            ++result.clipsCompressed;
                            continue;
                        }
                    }
                    if (WriteAll(to, data.data(), data.size()))
                        ++result.clips;
                    continue;
                }
                data.clear();
                std::uint64_t bytes = 0;
                if (CopyShared(from, to, input, bytes))
                    ++result.clips;
            }
        }

        // The sound reports the mod writes next to a clip (the recorder's and
        // the playback's sound-check) go with the clip: the triage reads the
        // generic families and the playback's per-family counts from them.
        void CopyDebugReports(const Input& input, Result& result, const std::vector<std::string>& clips)
        {
            // The mix probe's report too.
            static constexpr const char* kSuffixes[] = {".sound-generic.txt", ".sound-check.txt", ".sound-mix.txt"};
            const std::wstring target = input.reportFolder + L"\\debug";
            for (const std::string& clip : clips)
            {
                if (Cancelled(input))
                    return;
                if (clip.find('\\') != std::string::npos || clip.find('/') != std::string::npos || clip.find("..") != std::string::npos)
                    continue;
                const std::string stem = ClipStem(clip);
                for (const char* suffix : kSuffixes)
                {
                    const std::wstring name = Widen(stem + suffix);
                    const std::wstring from = FindData(input, L"ReplayDebug\\" + name);
                    if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES)
                        continue;
                    CreateDirectoryW(target.c_str(), nullptr);
                    std::uint64_t bytes = 0;
                    if (CopyShared(from, target + L"\\" + name, input, bytes))
                        ++result.debugFiles;
                }
            }
        }

        void CopyDumps(const Input& input, Result& result)
        {
            if (input.crashFolder.empty())
                return;
            std::wstring folder = input.crashFolder;
            if (folder.back() != L'\\')
                folder += L'\\';
            std::vector<WIN32_FIND_DATAW> files = List(folder + L"*");
            FILETIME nowFile{};
            GetSystemTimeAsFileTime(&nowFile);
            const std::uint64_t now = (static_cast<std::uint64_t>(nowFile.dwHighDateTime) << 32) | nowFile.dwLowDateTime;
            constexpr std::uint64_t kWeek = 7ull * 24 * 3600 * 10000000ull;
            const auto ticks = [](const FILETIME& t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
            files.erase(std::remove_if(files.begin(), files.end(),
                            [&](const WIN32_FIND_DATAW& f) {
                                return (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || now - ticks(f.ftLastWriteTime) > kWeek;
                            }),
                files.end());
            std::sort(files.begin(), files.end(),
                [&](const WIN32_FIND_DATAW& a, const WIN32_FIND_DATAW& b) { return ticks(a.ftLastWriteTime) > ticks(b.ftLastWriteTime); });
            if (files.size() > 10)
                files.resize(10);
            const std::wstring target = input.reportFolder + L"\\crashes";
            for (const WIN32_FIND_DATAW& file : files)
            {
                CreateDirectoryW(target.c_str(), nullptr);
                std::uint64_t bytes = 0;
                if (CopyShared(folder + file.cFileName, target + L"\\" + file.cFileName, input, bytes))
                    ++result.dumps;
            }
        }

        struct FileSink final : zip::Sink
        {
            HANDLE file = INVALID_HANDLE_VALUE;
            std::uint64_t at = 0;
            bool Write(const void* data, std::size_t size) override
            {
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                std::size_t done = 0;
                while (done < size)
                {
                    DWORD wrote = 0;
                    const DWORD want = static_cast<DWORD>((std::min<std::size_t>)(size - done, 1u << 24));
                    if (!WriteFile(file, bytes + done, want, &wrote, nullptr) || wrote != want)
                        return false;
                    done += wrote;
                }
                at += size;
                return true;
            }
            std::uint64_t Tell() const override { return at; }
            bool Patch(std::uint64_t offset, const void* data, std::size_t size) override
            {
                LARGE_INTEGER to{};
                to.QuadPart = static_cast<LONGLONG>(offset);
                DWORD wrote = 0;
                const bool ok = SetFilePointerEx(file, to, nullptr, FILE_BEGIN) &&
                    WriteFile(file, data, static_cast<DWORD>(size), &wrote, nullptr) && wrote == size;
                to.QuadPart = static_cast<LONGLONG>(at);
                return SetFilePointerEx(file, to, nullptr, FILE_BEGIN) && ok;
            }
        };

        void Walk(const std::wstring& folder, const std::wstring& relative, std::vector<std::pair<std::wstring, std::wstring>>& out)
        {
            for (const WIN32_FIND_DATAW& data : List(folder + L"\\*"))
            {
                const std::wstring path = folder + L"\\" + data.cFileName;
                const std::wstring name = relative.empty() ? std::wstring(data.cFileName) : relative + L"/" + data.cFileName;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    Walk(path, name, out);
                else
                    out.emplace_back(path, name);
            }
        }

        bool Deflatable(const std::wstring& path, const std::uint8_t* head, std::size_t headSize)
        {
            std::wstring lower = path;
            for (wchar_t& c : lower)
                c = static_cast<wchar_t>(towlower(c));
            const auto ends = [&](const wchar_t* suffix) {
                const std::size_t n = std::wcslen(suffix);
                return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
            };
            if (ends(L".jpg") || ends(L".png") || ends(L".zip"))
                return false;
            if (ends(L".rpclip"))
                return !clipcompress::IsCompressed(head, headSize);
            return true;
        }

        void DeleteTree(const std::wstring& folder)
        {
            for (const WIN32_FIND_DATAW& data : List(folder + L"\\*"))
            {
                const std::wstring path = folder + L"\\" + data.cFileName;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    DeleteTree(path);
                else
                    DeleteFileW(path.c_str());
            }
            RemoveDirectoryW(folder.c_str());
        }

        void Zip(const Input& input, Result& result)
        {
            const std::wstring game = GameFolder();
            HMODULE zlib = LoadLibraryExW((game + L"RedemptionEditor\\dlls\\z.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            zip::ZlibApi api;
            if (zlib)
            {
                api.deflateInit2_ = reinterpret_cast<decltype(api.deflateInit2_)>(GetProcAddress(zlib, "deflateInit2_"));
                api.deflate = reinterpret_cast<decltype(api.deflate)>(GetProcAddress(zlib, "deflate"));
                api.deflateEnd = reinterpret_cast<decltype(api.deflateEnd)>(GetProcAddress(zlib, "deflateEnd"));
            }
            result.zlib = api.Ready() ? "z.dll" : "none";
            std::vector<std::pair<std::wstring, std::wstring>> files;
            Walk(input.reportFolder, Widen(input.reportName), files);
            const std::wstring zipPath = input.dataRoot + L"ReplayReports\\" + Widen(ZipName(input.reportName, input.zipClip));
            const std::wstring temporary = zipPath + L".tmp";
            FileSink sink;
            sink.file = CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (sink.file == INVALID_HANDLE_VALUE)
            {
                result.zipError = "create_" + std::to_string(GetLastError());
                if (zlib)
                    FreeLibrary(zlib);
                return;
            }
            bool ok = true;
            {
                zip::Writer writer(sink, api.Ready() ? &api : nullptr, 6);
                std::vector<std::uint8_t> buffer(1 << 20);
                for (const auto& [path, name] : files)
                {
                    if (!ok)
                        break;
                    if (Cancelled(input))
                    {
                        result.zipError = "unload";
                        ok = false;
                        break;
                    }
                    HANDLE file = OpenShared(path);
                    if (file == INVALID_HANDLE_VALUE)
                        continue;
                    FILETIME written{}, local{};
                    SYSTEMTIME st{};
                    GetFileTime(file, nullptr, nullptr, &written);
                    FileTimeToLocalFileTime(&written, &local);
                    FileTimeToSystemTime(&local, &st);
                    std::uint16_t date = 0, time = 0;
                    zip::DosDateTime(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, date, time);
                    DWORD read = 0;
                    const bool first = ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != FALSE;
                    ok = first && writer.Begin(Narrow(name), Deflatable(path, buffer.data(), read), date, time);
                    while (ok && read > 0)
                    {
                        ok = writer.Write(buffer.data(), read) && !Cancelled(input);
                        if (ok && !ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
                            ok = false;
                    }
                    CloseHandle(file);
                    ok = ok && writer.End();
                }
                ok = ok && writer.Finish();
                if (!ok && result.zipError.empty())
                    result.zipError = writer.Error().empty() ? "read_failed" : writer.Error();
                result.zipEntries = static_cast<std::uint32_t>(writer.Entries());
                result.zipDeflated = static_cast<std::uint32_t>(writer.Deflated());
            }
            result.zipBytes = sink.at;
            CloseHandle(sink.file);
            if (zlib)
                FreeLibrary(zlib);
            if (!ok || !MoveFileExW(temporary.c_str(), zipPath.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                if (ok)
                    result.zipError = "rename_" + std::to_string(GetLastError());
                DeleteFileW(temporary.c_str());
                return;
            }
            result.zipped = true;
            result.zipPath = zipPath;
        }
    }

    std::string EnvModsLine()
    {
        const std::wstring game = LowerW(GameFolder());
        wchar_t windowsRaw[MAX_PATH]{};
        GetWindowsDirectoryW(windowsRaw, MAX_PATH);
        const std::wstring windows = LowerW(std::wstring(windowsRaw) + L"\\");
        wchar_t exeRaw[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exeRaw, MAX_PATH);
        const std::wstring exe = LowerW(exeRaw);
        HMODULE modules[1024];
        DWORD needed = 0;
        std::vector<std::string> mods, outside;
        bool vfs = false, modManager = false;
        if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        {
            const DWORD count = (std::min<DWORD>)(needed / sizeof(HMODULE), 1024);
            for (DWORD n = 0; n < count; ++n)
            {
                wchar_t raw[MAX_PATH]{};
                if (!GetModuleFileNameW(modules[n], raw, MAX_PATH))
                    continue;
                const std::wstring path = LowerW(raw);
                if (path == exe)
                    continue;
                const std::size_t slash = path.find_last_of(L"\\/");
                const std::wstring file = slash == std::wstring::npos ? path : path.substr(slash + 1);
                const std::string name = Narrow(std::wstring(raw).substr(slash == std::wstring::npos ? 0 : slash + 1));
                vfs = vfs || file == L"vfs.asi";
                modManager = modManager || file.find(L"modmanager") != std::wstring::npos;
                if (path.compare(0, game.size(), game) == 0)
                {
                    if (ListedModFile(name))
                        mods.push_back(Narrow(std::wstring(raw).substr(game.size())));
                }
                else if (path.compare(0, windows.size(), windows) != 0)
                    outside.push_back(name);
            }
        }
        const DWORD lmlAttributes = GetFileAttributesW((GameFolder() + L"lml").c_str());
        const bool lml = lmlAttributes != INVALID_FILE_ATTRIBUTES && (lmlAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (!modManager)
            for (const WIN32_FIND_DATAW& data : List(GameFolder() + L"ModManager*"))
                modManager = modManager || data.cFileName[0] != 0;
        return replay::qa::EnvModsLine(mods, outside, vfs, lml, modManager);
    }

    std::string GameExeVersion()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        return FileVersion(exe);
    }

    void Extend(const Input& input, Result& result)
    {
        try
        {
            WriteText(input.reportFolder + L"\\sysinfo.txt", SysInfo(input, result));
            std::vector<std::string> clips;
            MoveMarks(input, result, clips);
            for (const std::string& clip : input.clips)
                if (!clip.empty() && std::find(clips.begin(), clips.end(), clip) == clips.end())
                    clips.push_back(clip);
            CopyClips(input, result, clips);
            // The header names the marks the report took and the named clips it
            // could not take.
            if (!input.reportText.empty())
                WriteText(input.reportFolder + L"\\report.txt",
                    qareport::AddMissingClipsLine(qareport::ReplaceMarksLine(input.reportText, qareport::MarksText(result.markFolders)),
                        result.missingClips));
            CopyDebugReports(input, result, clips);
            CopyDumps(input, result);
            if (Cancelled(input))
            {
                result.zipError = "unload";
                return;
            }
            Zip(input, result);
            if (result.zipped)
                DeleteTree(input.reportFolder);
        }
        catch (const std::exception& exception)
        {
            result.error = std::string("exception_") + exception.what();
        }
        catch (...)
        {
            result.error = "unknown_exception";
        }
    }
}
