#pragma once

// Bench screenshot request, shared by ReplayProbe (script thread) and the
// ReplayEditor ReShade addon (present). Both live in RDR2.exe; the mapping
// is named per process. The probe writes the tag, then bumps `requested`;
// the addon saves a ReShade screenshot with the tag as postfix on the next
// present and publishes the same number in `saved`. No engine state is
// touched on the render thread: the addon only reads this block and calls
// ReShade.

#include <Windows.h>

#include <cstdint>
#include <cstdio>

namespace replay::bench
{
    constexpr std::uint32_t kShotVersion = 1;
    constexpr std::size_t kShotTagSize = 96;

    struct ShotShared
    {
        std::uint32_t version;
        std::uint32_t reserved;
        volatile LONG64 requested;
        volatile LONG64 saved;
        char tag[kShotTagSize];
    };

    inline void FormatShotMappingName(char* out, std::size_t size, DWORD pid)
    {
        std::snprintf(out, size, "Local\\RedemptionEditor.BenchShot.v1.%lu", static_cast<unsigned long>(pid));
    }

    // Bench request to close the editor. While the editor is interactive ReShade
    // holds the keyboard, so ScriptHook never sees the reload key. The addon owns
    // this auto-reset event; bench.py opens it by name and sets it before a reload.
    inline void FormatCloseEditorEventName(char* out, std::size_t size, DWORD pid)
    {
        std::snprintf(out, size, "Local\\RedemptionEditor.CloseEditor.v1.%lu", static_cast<unsigned long>(pid));
    }

    // Creates or opens the block for this process; nullptr on failure. The
    // mapping lives until the process exits.
    inline ShotShared* OpenShotShared()
    {
        char name[96];
        FormatShotMappingName(name, sizeof(name), GetCurrentProcessId());
        HANDLE mapping = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ShotShared), name);
        if (!mapping)
            return nullptr;
        auto* shared = static_cast<ShotShared*>(
            MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShotShared)));
        if (!shared)
        {
            CloseHandle(mapping);
            return nullptr;
        }
        if (shared->version == 0)
            shared->version = kShotVersion;
        return shared->version == kShotVersion ? shared : nullptr;
    }
}
