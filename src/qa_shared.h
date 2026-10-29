#pragma once

// QA kit: the block ReplayProbe (the script thread) and the ReplayEditor
// add-on (the render thread) share for the "mark problem" key, beside the
// editor bridge (same pattern as bench_shot.h: a named mapping per process,
// created by whichever side comes first, never unmapped while the process
// lives).
//
// Core to add-on: whether the QA screenshot ring may run (a clip plays, no
// export, no recording), and each mark (markSequence grows; markId and the
// folder for its pictures). Add-on to core: the tester's note (Enter in
// the note box; noteSequence grows), the pictures written for a mark and
// the ring's cost (QA_HEALTH_SUMMARY). Plain fields under a sequence: the
// writer fills them, then bumps the sequence with an interlocked write.
//
// No function pointer and no engine state crosses: either module can be
// unloaded at any time.

#include <Windows.h>

#include <cstdint>
#include <cstdio>

namespace replay::qa
{
    inline constexpr std::uint32_t kSharedVersion = 1;
    inline constexpr std::size_t kNoteCapacity = 160; // UTF-8, with the terminator
    inline constexpr std::size_t kFolderCapacity = 520; // UTF-8 absolute path

    struct Shared
    {
        std::uint32_t version;
        std::uint32_t structSize;
        // Core -> add-on.
        volatile LONG ringAllowed;   // 1: a clip plays (not exporting, not recording) and QaMarks=1
        volatile LONG marksEnabled;  // QaMarks in the ini (the key bar shows Shift+F2)
        volatile LONG64 markSequence;
        std::uint32_t markId;
        std::uint32_t markPlayheadMs;
        char markFolder[kFolderCapacity];
        // Add-on -> core: the note.
        volatile LONG64 noteSequence;
        std::uint32_t noteMarkId;
        std::uint32_t noteAnswered; // 1 Enter (with or without text), 0 Esc
        char note[kNoteCapacity];
        // Add-on -> core: the pictures of a mark.
        volatile LONG64 shotsSequence;
        std::uint32_t shotsMarkId;
        std::uint32_t shotsRing;
        std::uint32_t shotsFull;
        std::uint32_t reserved0;
        char shotsError[64];
        // Add-on -> core: the ring's cost since the playback started
        // (render thread microseconds per capture; frame times around it).
        volatile LONG64 statsSequence;
        std::uint32_t ringCaptures;
        std::uint32_t ringIssueUsAvg;
        std::uint32_t ringIssueUsMax;
        std::uint32_t ringWorkerUsAvg;   // map + reduce + copy on the worker
        std::uint32_t frameUsAvg;        // all frames of the playback
        std::uint32_t frameUsAfterAvg;   // the 3 frames after each capture
        std::uint32_t frameUsAfterMax;
        std::uint32_t ringPath;          // 1 GPU blit (Vulkan), 2 full copy (D3D12), 0 none
        std::uint32_t ringWidth;
        std::uint32_t ringHeight;
        // Add-on -> core: GetTickCount64 of its last frame (0: never), so a
        // mark does not wait for a note or pictures nobody will send.
        volatile LONG64 addonFrameAt;
        // Add-on -> core: the note box is open (the core keeps its keys
        // off the camera, as with the editor open).
        volatile LONG noteOpen;
        // Core -> add-on: the mark folders in ReplayReports\marks not sent yet
        // (Shift+F1 moves them into the report), for the key bar's "Send report
        // (N)" and the reminders. Takes the place of a reserved field: same
        // layout, no version change.
        volatile LONG pendingMarks;
        // Add-on -> core: Shift+F2 seen by the add-on while the editor has
        // the keyboard (ScriptHook does not get keys then); the core marks
        // as for its own key.
        volatile LONG64 markRequestSequence;
        // Add-on -> core: Shift+F1 (the report) seen the same way.
        volatile LONG64 reportRequestSequence;
    };

    inline void FormatSharedName(char* out, std::size_t size, DWORD pid)
    {
        std::snprintf(out, size, "Local\\RedemptionEditor.QaMarks.v1.%lu", static_cast<unsigned long>(pid));
    }

    // Creates or opens the block for this process; nullptr on failure. The
    // mapping lives until the process exits (both sides keep the view).
    inline Shared* OpenShared()
    {
        char name[96];
        FormatSharedName(name, sizeof(name), GetCurrentProcessId());
        HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name);
        if (!mapping)
            return nullptr;
        auto* shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (!shared)
        {
            CloseHandle(mapping);
            return nullptr;
        }
        if (shared->version == 0)
        {
            shared->structSize = sizeof(Shared);
            shared->version = kSharedVersion;
        }
        return shared->version == kSharedVersion && shared->structSize == sizeof(Shared) ? shared : nullptr;
    }

    inline LONG64 ReadSequence(volatile LONG64* value) { return InterlockedCompareExchange64(value, 0, 0); }
}
