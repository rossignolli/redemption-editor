#pragma once

// The host's log: RedemptionEditor\host.log next to the host folder
// (<game>\RedemptionEditor\host\RedemptionEditorHost.dll writes
// <game>\RedemptionEditor\host.log), one line per call in ReShade.log's
// format ("HH:MM:SS:mmm [tid] | INFO  | text"). Thread safe. The core's
// reshade::log::message lines come here too (ReShadeLogMessage export).

namespace host::log
{
    enum class Level
    {
        Error = 1,
        Warning = 2,
        Info = 3,
        Debug = 4,
    };

    void Write(Level level, const char* text);
    void Printf(Level level, const char* format, ...);

    // HOST_UNSUPPORTED <name>, once per name.
    void UnsupportedOnce(const char* name);
}

#define HOST_LOG(...) ::host::log::Printf(::host::log::Level::Info, __VA_ARGS__)
#define HOST_WARN(...) ::host::log::Printf(::host::log::Level::Warning, __VA_ARGS__)
#define HOST_ERROR(...) ::host::log::Printf(::host::log::Level::Error, __VA_ARGS__)
