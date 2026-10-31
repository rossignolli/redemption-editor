// The host's log (host_log.h), the reshade::log shim of ReShade's Vulkan code
// (shims/dll_log.hpp) and the two ReShade exports the core looks up in the
// host DLL (EditorCoreSetApiModule points it here): ReShadeLogMessage and
// RedemptionEditorHostVersion (never ReShadeVersion, see below). ReShadeRegisterAddon is deliberately NOT exported: a
// ReShade add-on (ours under Host=reshade, or any other) finds its ReShade by
// that export, and must never find the host instead.

#include "host_log.h"
#include "shims/dll_log.hpp"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

extern "C" IMAGE_DOS_HEADER __ImageBase;

// The API the host implements is ReShade API 18 (the header the core is built
// with); render_export/reshade_query_abi.h maps 6.0 to 6.7 to API 18.
// Never named ReShadeVersion: ReShade's DllMain refuses to start when another
// module exports that name ("Another ReShade instance was already loaded"),
// which turned off the player's ReShade and its effects (seen in game).
extern "C" __declspec(dllexport) const char* RedemptionEditorHostVersion = "6.7.0 RedemptionEditorHost";

namespace host::log
{
    namespace
    {
        std::mutex g_mutex;
        HANDLE g_file = INVALID_HANDLE_VALUE;
        bool g_tried = false;
        std::set<std::string> g_unsupported;

        // <dll folder>\..\host.log
        std::wstring LogPath()
        {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
            std::wstring folder = path;
            folder = folder.substr(0, folder.find_last_of(L"\\/"));          // ...\RedemptionEditor\host
            folder = folder.substr(0, folder.find_last_of(L"\\/") + 1);      // ...\RedemptionEditor\ .
            return folder + L"host.log";
        }

        void OpenLocked()
        {
            if (g_tried)
                return;
            g_tried = true;
            g_file = CreateFileW(LogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }

    void Write(Level level, const char* text)
    {
        static constexpr char kNames[][6] = {"ERROR", "WARN ", "INFO ", "DEBUG"};
        int index = static_cast<int>(level) - 1;
        if (index < 0)
            index = 0;
        if (index > 3)
            index = 3;
        SYSTEMTIME time{};
        GetLocalTime(&time);
        char head[64];
        const int headLength = std::snprintf(head, sizeof(head), "%02hu:%02hu:%02hu:%03hu [%5lu] | %.5s | ", time.wHour, time.wMinute,
            time.wSecond, time.wMilliseconds, GetCurrentThreadId(), kNames[index]);
        std::string line(head, headLength > 0 ? static_cast<std::size_t>(headLength) : 0);
        line += text ? text : "";
        line += "\r\n";
        std::lock_guard<std::mutex> lock(g_mutex);
        OpenLocked();
        if (g_file == INVALID_HANDLE_VALUE)
            return;
        DWORD written = 0;
        WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }

    void Printf(Level level, const char* format, ...)
    {
        char text[2048];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        Write(level, text);
    }

    void UnsupportedOnce(const char* name)
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_unsupported.insert(name).second)
                return;
        }
        Printf(Level::Warning, "HOST_UNSUPPORTED %s", name);
    }
}

// ReShade's Vulkan implementation logs through this (shims/dll_log.hpp).
void reshade::log::message(level level, const char* format, ...)
{
    char text[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    host::log::Write(static_cast<host::log::Level>(level), text);
}

// reshade::log::message of the core (reshade.hpp looks it up by name).
extern "C" __declspec(dllexport) void ReShadeLogMessage(HMODULE, int level, const char* message)
{
    host::log::Write(static_cast<host::log::Level>(level), message);
}
