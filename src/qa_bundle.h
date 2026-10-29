#pragma once

// QA kit: what the QA report (Shift+F1) adds to
// its folder after the log copies, and the zip. Runs on the report's
// thread (script.cpp StartQaReport): Win32 only, no engine, no native.
//
//   <report>\sysinfo.txt   Windows, CPU, RAM, GPUs and drivers, the game's
//                          exe version and SHA-256, ScriptHook, ReShade,
//                          the mod's files and SHA-256, the other mods of
//                          the game folder
//   <report>\marks\...     every ReplayReports\marks\mark-* folder (moved)
//   <report>\marks.json    the marks' mark.json in one array
//   <report>\clips\...     the clips the marks name and the open clip; a
//                          plain clip is compressed with the mod's own clip
//                          compression (still a .rpclip the editor opens)
//   <report>\crashes\...   the newest minidumps of
//                          %LOCALAPPDATA%\RedemptionEditor\crashes (7 days)
//   <report>\debug\...     ReplayDebug\<clip>.sound-generic.txt
//                          and <clip>.sound-check.txt of the clips above
//
// Then ReplayReports\<report>-<clip>.zip (qa_zip.h, deflate through z.dll
// of RedemptionEditor\dlls when it loads, stored otherwise); the folder is
// deleted when the zip is whole and kept when it is not.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::qa::bundle
{
    struct Input
    {
        std::wstring reportFolder;          // ReplayReports\report-... (relative to the game folder, the working directory)
        std::string reportName;             // report-...
        std::vector<std::string> clips;     // file names in ReplayClips (marked first, then the open clip)
        std::string zipClip;                // the clip in the zip's name ("" none)
        std::wstring crashFolder;           // the crash handler's dump folder ("" unknown)
        std::string asiVersion;
        std::string asiSha256;
        std::wstring asiPath;
        int scriptHookGameVersion = -1;     // ScriptHook's getGameVersion()
        int clipCompressLevel = 9;
        const std::atomic<bool>* cancel = nullptr;
        // report.txt as the report thread wrote it; once the
        // mark folders are moved its "marks:" line is rewritten from them.
        std::string reportText;
        // The base folder of the mod's writes (storage_paths.h, ending in '\\';
        // empty: the working directory): the marks are there and the zip goes
        // there. The clips and the debug reports are looked up in each of
        // readRoots in turn (empty: the working directory only).
        std::wstring dataRoot;
        std::vector<std::wstring> readRoots;
    };

    struct Result
    {
        bool zipped = false;
        std::wstring zipPath;               // relative, as the notice shows it
        std::uint64_t zipBytes = 0;
        std::uint32_t zipEntries = 0;
        std::uint32_t zipDeflated = 0;
        std::uint32_t marks = 0;
        std::vector<std::string> markFolders; // the folders moved, in listing order
        std::uint32_t debugFiles = 0;         // ReplayDebug sound reports of the clips, copied to the debug folder
        std::uint32_t clips = 0;
        std::uint32_t clipsCompressed = 0;
        std::vector<std::string> missingClips; // named, not in ReplayClips
        std::uint32_t dumps = 0;
        std::string zlib;                   // "z.dll" loaded, "none" stored only
        std::string error;                  // first failure of the extension ("" none)
        std::string zipError;
        std::string gameExeVersion;
        int gameBuildMatches = -1;
    };

    void Extend(const Input& input, Result& result);

    // RDR2.exe's file version ("1.0.1491.50"; "none" unreadable).
    std::string GameExeVersion();

    // The ENV_MODS line of the modules loaded now (qa_marks.h EnvModsLine);
    // also in sysinfo.txt.
    std::string EnvModsLine();
}
