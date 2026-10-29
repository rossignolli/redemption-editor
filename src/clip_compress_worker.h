#pragma once

// Background clip compressor.
// One plain Win32 thread (CreateThread: std::thread pins the module and
// breaks Ctrl+R, see pose_capture.cpp), low priority, that compresses the
// clips of ReplayClips not written nor opened for N days
// (clipcompress::OldEnough) with clipcompress::CompressFile (the packed
// container with LZMA; the zstd clips of earlier builds and the clips a
// save wrote fast packed, zstd blocks, are repacked; the LZMA ones are
// left). A pass runs
// some seconds after the mod loads (when automatic compression is on), then
// every half hour, and when the editor asks ("Compress old clips now").
//
// The script thread tells it when a clip is being saved or loaded (the
// file in progress is cancelled and retried after) and which clip is open
// (never compressed while open). Log lines are queued for the script
// thread, which owns the log file.

#include "clip_compress.h"

#include <cstdint>
#include <string>
#include <vector>

namespace replay::clipcompress::worker
{
    struct Settings
    {
        bool automatic = true;
        std::uint32_t days = kDefaultDays;
        int level = 19;
        // The packed container (clip_pack.h) by default; zstd as before 1.0
        // with the dev ini key CompressFormat=1.
        Format format = Format::Packed;
        bool upgrade = true; // packed: zstd clips and fast packed ones are repacked too
    };

    // Starts the thread once (a second call only applies the settings).
    // `folder` is the clips folder; the first automatic pass waits delayMs.
    void Start(const std::wstring& folder, const Settings& settings, std::uint32_t delayMs);
    void Configure(const Settings& settings);
    // A pass now over the clips older than the days, automatic or not.
    void RequestPass();
    // One clip now, whatever its age (a file name in the folder).
    void RequestFile(const std::wstring& name);
    // True while a clip file is being saved or loaded.
    void SetHold(bool hold);
    // The clip open in the editor (a path or a file name; compared by name).
    void SetOpenClip(const std::wstring& path);
    // The file being compressed now (a file name, empty when none).
    std::wstring FileInWork();

    enum class State : std::uint32_t
    {
        Off = 0,     // no thread
        Idle = 1,
        Running = 2, // a pass in progress
        Held = 3     // waiting for a save or a load
    };
    struct Status
    {
        State state = State::Off;
        std::uint32_t done = 0;  // files handled in the current or last pass
        std::uint32_t total = 0; // candidates of that pass
        std::uint32_t compressed = 0;
    };
    Status CurrentStatus();
    // Grows each time a file was replaced (the clip list is rebuilt).
    std::uint64_t ChangeSequence();
    // The log lines written since the last call.
    void DrainLog(std::vector<std::string>& lines);

    // Module unload: the flag, then a bounded wait for the thread to leave
    // its loop (never a join, DllMain holds the loader lock), then detach.
    void StopForUnload();
}
