#pragma once

// Render export: the probe side. Owns the export channel
// (src/export_channel.h), the export clock (src/export_schedule.h) and the game
// timer lock (src/export_timer.h), and gives the transport its clock while an
// export runs. Off unless RedemptionEditor.ini [Export] Enabled=1: then
// Configure does not even create the channel and every call below is a no-op.
//
// script.cpp calls, per tick, around the transport update:
//   BeginTick(input)                the tick's plan (and the state machine)
//   TransportNowMs(wallClockMs)     the transport's clock (the wall clock when idle)
//   EndTick(playhead, scaleAhead)   the tick's stamp for the add-on, the next step
//                                   (and the next frame's blur samples)
// Script fiber only.

#include "export_schedule.h"

#include <cstdint>
#include <ostream>
#include <string>

namespace replay::exportprobe
{
    void Configure(const std::string& iniPath);
    bool Enabled();
    bool Active(); // an export owns the transport clock

    // Editor commands (CommandType::StartExport / CancelExport).
    // `outputFrames` gives the span's output frames at the playback's speeds
    // (exporting::SpanOutputFrames); null counts them at 1x.
    using OutputFramesFn = std::uint64_t (*)(std::uint64_t startMs, std::uint64_t endMs, replay::exporting::FrameRate rate);
    // `earliestMs`: the clip's opening hold (the transport's start): a range
    // that starts before it starts there (the playhead cannot stand earlier,
    // and the frames counted are the ones the capture will take).
    void Request(std::uint32_t requestSeq, std::ostream& log, OutputFramesFn outputFrames = nullptr, std::uint64_t earliestMs = 0);
    void Cancel(const char* reason, std::ostream& log);

    // Whole project export (script.cpp drives it, one clip after the other):
    // piece `index` of `count`, the clip's range [startMs, endMs] with the
    // editor's other settings. framesBefore: the frames of the pieces done;
    // framesAfter: the plan's for the pieces after this one (progress only).
    // `thumbnail`: this piece holds the whole video's middle frame.
    struct Segment
    {
        std::uint32_t index = 0;
        std::uint32_t count = 0;
        std::uint64_t startMs = 0;
        std::uint64_t endMs = 0;
        std::uint64_t earliestMs = 0;
        std::uint64_t framesBefore = 0;
        std::uint64_t framesAfter = 0;
        bool thumbnail = false;
    };
    // False when the export is off, busy with something else, or the range
    // is empty (nothing was requested then).
    bool RequestSegment(std::uint32_t requestSeq, const Segment& segment, std::ostream& log, OutputFramesFn outputFrames);
    // The project export begins: SegmentDone (busy, cancellable) before the
    // first piece while its clip opens; the progress fields from the plan.
    void BeginProject(std::uint32_t count, std::uint64_t framesPlanned, std::ostream& log);
    // Between two pieces: the one opening next and the frames of the ones done.
    void ProjectProgress(std::uint32_t nextIndex, std::uint64_t framesBefore);
    // The channel's state (exportchannel::State as a number; 0 when off) and
    // the frames the last piece encoded.
    std::uint32_t StateValue();
    std::uint64_t FramesEncoded();
    // The project export refused before or between pieces (a clip missing,
    // a clip that does not open): Failed with `error`
    // (render_export::ExportError), whatever state it was in.
    void FailProject(std::uint32_t error, const char* reason, std::ostream& log);

    struct TickInput
    {
        std::uint64_t playheadMs = 0;
        std::uint64_t durationMs = 0;
        bool settling = false;   // seek settle or clip load still running
        float frameTime = 0.0f;  // GET_FRAME_TIME this tick (the timer check)
        float worldScale = 1.0f; // the SET_TIME_SCALE the replay applies
    };

    // What the tick must do before the transport update.
    struct TickPlan
    {
        bool active = false;      // the export clock drives this tick
        bool holdTime = false;    // time held (seeking, settling, waiting for the add-on)
        bool hideHud = false;     // no game HUD, debug drawing or text notices in the picture
        bool seekToStart = false; // seek to startMs this tick (then settle)
        bool resume = false;      // the transport plays (unpaused)
        bool pause = false;       // the transport pauses
        bool drawTag = false;     // draw the latency tag of tagSerial (export_channel.h)
        std::uint64_t tagSerial = 0;
        std::uint64_t startMs = 0;
    };
    TickPlan BeginTick(const TickInput& input, std::ostream& log);
    std::uint64_t TransportNowMs(std::uint64_t wallClockMs);
    // `worldScaleAhead`: the lowest world time scale the playback will apply
    // over the next two video frames (script.cpp ExportWorldScaleAhead). A
    // frame starting on the next tick takes the most motion blur samples
    // whose world step stays at or above the engine's minimum.
    void EndTick(std::uint64_t playheadMs, float worldScaleAhead, std::ostream& log);
    // The frame rate of the export being run (the look ahead of the blur).
    replay::exporting::FrameRate ExportRate();
    // The Snapshot flag (SnapshotExporting): any state but idle.
    bool ShowsExporting();
    // The picture belongs to the video (seek to the end of the flush):
    // the HUD, the debug drawing and the text notices stay off.
    bool HidesHud();

    // Bench (scenario op "export", tools/bench): the settings straight into
    // the channel, as the editor's dialog writes them; false when the export
    // is off or busy. BenchPoll: 0 running, 1 done, -1 failed or cancelled.
    bool BenchRequest(const void* settings, std::uint32_t settingsBytes, std::ostream& log);
    int BenchPoll();
    void BenchCancel(std::ostream& log);

    // Unload and hot reload: timer released, sound back, channel marked Cancelled.
    void Shutdown();
}
