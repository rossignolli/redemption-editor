#pragma once

// Render export: the export finisher's decisions (render_export.cpp
// FinishWorker), kept apart so they can be tested. Pure (unit tested in
// ReplayCoreTests).

#include "export_channel.h"

#include <string>

namespace render_export
{
    // The finisher's wait for the probe's audio pass.
    enum class AudioWaitEnd
    {
        Waiting = 0,  // still running
        AudioDone,    // the pass ended: the WAV is attached
        ProbeFailed,  // the probe failed the export (no capture acknowledgement in time)
        Cancelled,    // the export was cancelled during the pass: it fails, the video stays under its temporary name
        ProbeEnded,   // reset by the probe: the video is kept without sound, as before
        Deadline,     // the pass never ended (the wait's own limit)
        Stopped       // Shutdown asked the finisher to stop (the editor or the game is closing)
    };

    // One look at the wait: `stopRequested` first (it must end the wait even
    // when no game tick moves the probe any more).
    inline AudioWaitEnd AudioWaitStep(replay::exportchannel::State state, bool deadlinePassed, bool stopRequested)
    {
        using replay::exportchannel::State;
        if (stopRequested)
            return AudioWaitEnd::Stopped;
        if (state == State::AudioDone)
            return AudioWaitEnd::AudioDone;
        if (state == State::Failed)
            return AudioWaitEnd::ProbeFailed;
        if (state == State::Cancelled)
            return AudioWaitEnd::Cancelled;
        if (state == State::Off || state == State::Done)
            return AudioWaitEnd::ProbeEnded;
        if (deadlinePassed)
            return AudioWaitEnd::Deadline;
        return AudioWaitEnd::Waiting;
    }

    // The outcome of the finalize step (a remux, or a plain rename of the
    // video pass's temporary file to the requested name).
    struct FinalizeReport
    {
        bool ok = false;     // the requested file exists (Done); else Failed
        bool warning = false; // ok, but without the audio and chapters
        std::string detail;  // the dialog's text: the file, a warning, or where the video is kept
    };

    // `remuxNeeded`: the finalize had to remux (audio, chapters, a fragmented
    // muxer); `finalized`: that remux succeeded; `moved`: the rename of the
    // temporary file to `finalPath` succeeded (the remux's fallback, or the
    // plain rename when no remux was needed). Paths in UTF-8.
    inline FinalizeReport ReportFinalize(bool remuxNeeded, bool finalized, bool moved, const std::string& finalPath, const std::string& tempPath)
    {
        FinalizeReport report;
        if (remuxNeeded && finalized)
        {
            report.ok = true;
            report.detail = finalPath;
        }
        else if (moved)
        {
            report.ok = true;
            report.warning = remuxNeeded;
            report.detail = remuxNeeded ? finalPath + " (warning: the finalize failed, so the video has no sound and no chapters; see the .ffmpeg.log)"
                                        : finalPath;
        }
        else
        {
            report.detail = std::string(remuxNeeded ? "finalize_failed" : "rename_failed") +
                ": the video could not be saved as " + finalPath + "; it is kept at " + tempPath;
        }
        return report;
    }
}
