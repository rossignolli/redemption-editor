#pragma once

// QA kit: the core side of "mark problem"
// (Shift+F2 during a replay playback) and of the rolling health buffer.
//
// Script thread, except OnLogLine (any thread that writes the mod's log)
// and RequestMark (the keyboard callback). The pure parts are in
// qa_health.h and qa_marks.h; the pictures are the editor add-on's
// (editor/qa_shots.h, through qa_shared.h); the report bundle is
// qa_bundle.h.

#include <atomic>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace replay::entity
{
    class LifecycleTrack;
}

namespace replay::playback
{
    struct Puppets;
}

namespace replay::qa::kit
{
    // QaMarks in the ini (default 1): the key, the health buffer's marks
    // and the editor's screenshot ring. The health summary line is written
    // either way.
    // Also writes ASI_GAME (RDR2.exe's file version against build 1491.50);
    // on another build a notice tells the tester once, in game.
    void Configure(const std::string& iniPath, std::ostream& log, int scriptHookGameVersion);

    // Every log line, before the Debug=0 filter (script.cpp QuietFilter).
    // Cheap: only lines starting with 'P' while a clip plays are looked at.
    void OnLogLine(std::string_view line);

    // The keyboard callback (Shift+F2): handled on the next tick.
    void RequestMark();
    // How many marks were pressed since the mod loaded (the mix probe logs
    // its window around each new one).
    std::uint64_t MarkPresses();

    struct TickState
    {
        bool playing = false;
        bool paused = false;
        bool scrubbing = false;
        bool settling = false;   // a seek settle or a clip load wait: a long tick there is by design
        bool exporting = false;
        bool recording = false;
        bool editorOpen = false;
        bool freeCam = false;
        bool editingMarker = false;
        std::uint64_t playheadMs = 0;
        std::uint64_t durationMs = 0;
        double rate = 1.0;
        std::string clip;        // the open clip's file name ("" the buffer's, unsaved)
        const playback::Puppets* puppets = nullptr;
        // The open clip's attachments, so a mark lists every puppet attached
        // to a body (a held object, a horse's cargo).
        const entity::LifecycleTrack* lifecycle = nullptr;
    };

    void Tick(std::uint64_t now, const TickState& state);

    // Shift+F1 the add-on saw while the editor had the keyboard (once).
    bool TakeReportRequest();

    // The report (Shift+F1) waits while a mark's window is open or its
    // note and pictures may still come (at most ~20 s after the press).
    bool ReadyForReport(std::uint64_t now);
    // Marks of this session not yet in a report (their folders), and the
    // clips they name.
    std::vector<std::string> PendingMarkFolders();
    std::vector<std::string> MarkedClips();
    // The report took the marks' folders.
    void MarksReported();

    // Set on unload: the report thread's clip compression stops.
    std::atomic<bool>& CancelFlag();
    void Shutdown();
}
