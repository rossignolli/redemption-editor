#pragma once

// Render export: the add-on side state machine. It reads the probe's export
// channel (src/export_channel.h), takes one picture per present at the
// capture point (reshade_overlay, before the editor's UI is drawn), feeds
// the GPU exporter (gpu_export.h: copy, motion blur accumulation, colour
// conversion, staging ring) with the Capture ticks, streams the frames to
// the encoder (libav_encoder.h: the FFmpeg libraries in process, from
// RedemptionEditor\dlls; or the optional ffmpeg.exe pipe of encoder_sink.h),
// runs the audio pass capture and the finalize (audio, chapters, thumbnail)
// on the finisher thread, which alone owns that capture. Off unless RedemptionEditor.ini [Export]
// Enabled=1, and inert while the channel's state is Off.
//
// Wiring (replay_editor_addon.cpp):
//   OnOverlay  -> ExportController::OnOverlay, before seek_freeze::Update
//   OnPresent  -> ExportController::OnPresent
//   OnDestroyRuntime / EditorCoreStop -> ExportController::Shutdown
//   the Export screen -> RequestExport / Cancel / Status
//
// Render thread only (except Status, a copy of atomics and strings under a lock).

#include "audio_sink.h"
#include "encoder_sink.h"
#include "export_finish.h"
#include "export_types.h"
#include "ffmpeg_args.h"
#include "gpu_export.h"
#include "libav_encoder.h"
#include "project_parts.h"

#include <reshade.hpp>

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace render_export
{
    struct ExportStatus
    {
        replay::exportchannel::State state = replay::exportchannel::State::Off;
        std::uint64_t framesEncoded = 0;
        std::uint64_t framesTotalHint = 0;
        std::uint32_t queueDepth = 0;
        std::uint32_t latencyPresents = ~0u;
        std::uint32_t timerMismatches = 0;
        ExportError error = ExportError::None;
        double secondsElapsed = 0.0;
        double secondsLeft = -1.0; // -1 unknown
        std::string detail;        // the last error or the finished file (UTF-8)
        bool channel = false;      // the probe's export channel is there ([Export] Enabled=1 in the probe)
        GpuTimings timings;
        // A whole project export: piece segmentIndex (0 based) of segmentCount
        // (0: one range of a clip), the frames of the pieces done and the
        // whole's estimate.
        std::uint32_t segmentIndex = 0;
        std::uint32_t segmentCount = 0;
        std::uint64_t projectFramesBefore = 0;
        std::uint64_t projectFramesTotal = 0;
    };

    class ExportController
    {
    public:
        static ExportController& Get();

        // RedemptionEditor.ini [Export]: Enabled (on by default), Backend (libav,
        // the default, or ffmpeg), Encoder (auto, nvenc, amf, qsv, cpu),
        // FfmpegDlls (default <game>\RedemptionEditor\dlls), Ffmpeg (the
        // ffmpeg.exe of the fallback backend), Folder, AssumeLatency, Slots.
        void LoadConfig();
        bool Enabled() const { return m_enabled; }
        const std::wstring& Folder() const { return m_folder; }

        // The encoders of this PC (the dialog): StartProbe loads the FFmpeg
        // DLLs and opens every variant once, on a thread (a second or so).
        void StartProbe();
        bool ProbeDone() const;
        // -1 while probing or when no variant of the preset opened.
        int VariantFor(replay::exportchannel::VideoPreset preset) const;
        bool PresetAvailable(replay::exportchannel::VideoPreset preset) const;
        // "NVENC", "CPU"...; "ffmpeg.exe" for the fallback backend.
        std::string BackendLabel(replay::exportchannel::VideoPreset preset) const;
        // Why the export cannot run (no DLLs and no ffmpeg.exe), or empty.
        std::string EncoderProblem() const;
        // Why this ReShade cannot run the export (its version is not one the
        // query reads are known for, reshade_query_abi.h), or empty.
        std::string ReShadeProblem() const;
        bool UsesFfmpegExe() const { return m_useFfmpegExe; }

        // The Export screen: settings into the channel (the editor then sends
        // CommandType::StartExport with the returned request sequence).
        // `chapters`: marker times from the export's start (ms).
        bool RequestExport(const replay::exportchannel::Settings& settings, const std::vector<Chapter>& chapters, std::uint32_t& requestSeq);
        // A whole project export: the same, the chapters one per clip
        // (`clipTitles`, in the project's order, at each piece's first frame).
        // The editor then sends StartExport with enabled 1.
        bool RequestProjectExport(const replay::exportchannel::Settings& settings, const std::vector<std::string>& clipTitles, std::uint32_t& requestSeq);
        void Cancel();
        ExportStatus Status() const;
        bool Busy() const;

        void OnOverlay(reshade::api::effect_runtime* runtime);
        void OnPresent(reshade::api::effect_runtime* runtime);
        // Runtime destroyed or the core swapped: `runtime` (may be null)
        // waits for the GPU before the ring is freed.
        void Shutdown(reshade::api::effect_runtime* runtime);

    private:
        bool OpenChannel();
        void Arm(reshade::api::effect_runtime* runtime);
        void Capture(reshade::api::effect_runtime* runtime, replay::exportchannel::State state);
        void DrainFrames(reshade::api::effect_runtime* runtime, bool wait);
        void PublishTimings();
        void Fail(ExportError error, const std::string& detail);
        void StartFinish();
        void FinishWorker();
        void Teardown(reshade::api::effect_runtime* runtime);
        void Log(const std::string& line) const;
        // Whole project export (finisher thread): the pieces joined into the
        // requested file (sound, chapters), then the pieces deleted.
        bool FinishProject(bool libav, std::string& detail, std::string& doneDetail, ExportError& failure);
        // The pieces of a project export that will not be joined (cancelled,
        // failed): deleted. Render thread or finisher; never while an encoder
        // still writes one (the closer is joined first).
        // joinCloser false on the finisher (the render thread owns the closer).
        void DropProjectParts(const char* why, bool joinCloser = true);

        bool EnsureRuntime();

        bool m_enabled = false;
        bool m_useFfmpegExe = false;    // [Export] Backend=ffmpeg, or the DLLs missing and Ffmpeg= set
        bool m_backendLibav = true;     // [Export] Backend (default libav)
        bool m_automaticBackend = true; // [Export] Encoder=auto
        EncoderBackend m_forcedBackend = EncoderBackend::Nvenc;
        std::wstring m_dllFolder;
        bool m_ffmpegConfigured = false; // [Export] Ffmpeg= given
        mutable std::mutex m_probeMutex;
        std::thread m_probeThread;
        bool m_probeStarted = false;
        ProbeResult m_probe;
        std::string m_runtimeError;
        bool m_encoderLogged = false;
        std::wstring m_ffmpeg = L"ffmpeg.exe";
        std::wstring m_folder;
        std::uint32_t m_assumedLatency = ~0u;
        std::uint32_t m_slots = 6;
        bool m_hdr10 = false; // [Export] Hdr10: a 10 bit back buffer holds PQ
        int m_audioOffsetMs = 0; // [Export] AudioOffsetMs: added to the audio pass alignment

        HANDLE m_mapping = nullptr;
        replay::exportchannel::Channel* m_channel = nullptr;
        std::uint32_t m_openRetry = 0;
        std::uint32_t m_configRetry = 0;

        std::atomic<bool> m_armed{false};     // the finisher thread clears it
        std::atomic<bool> m_finishing{false};
        std::uint64_t m_presentIndex = 0; // overlays since armed (0 based)
        std::uint32_t m_latency = ~0u;
        std::uint32_t m_latencyVotes = 0;
        std::uint32_t m_latencyCandidate = ~0u;
        std::uint64_t m_lastSerial = ~0ull;
        std::uint64_t m_samplesTaken = 0;
        std::uint64_t m_framesQueued = 0;
        std::uint32_t m_width = 0;
        std::uint32_t m_height = 0;
        LARGE_INTEGER m_startQpc{};
        double m_ringWaitUs = 0.0;
        double m_queueWaitUs = 0.0;

        GpuExporter m_gpu;
        std::unique_ptr<EncoderSink> m_encoder;
        EncodeJob m_job;
        std::vector<Chapter> m_chapters;
        replay::exportchannel::Settings m_settings;
        std::thread m_finisher;
        // Set by Shutdown before it joins the finisher: its audio pass wait
        // ends at once (no game tick has to acknowledge it).
        std::atomic<bool> m_stopFinisher{false};
        std::thread m_closer; // closes a failed export's encoder
        std::uint32_t m_teardownPresents = 0;
        // The teardown follows a failure or a cancel: the GPU may still run
        // the export's commands, so Teardown waits for the queue first.
        bool m_teardownAfterFail = false;

        mutable std::mutex m_statusMutex;
        std::string m_detail;

        // Whole project export: set at the first piece's Arm.
        struct Project
        {
            bool active = false;
            std::uint32_t count = 0;
            std::uint32_t index = 0;   // the piece armed last
            std::wstring finalOutput;  // the requested file (or folder)
            std::vector<std::string> titles;
            std::vector<ProjectPart> parts; // the pieces done
            std::vector<std::wstring> stray; // a piece's files that never made it into `parts`
        };
        Project m_project;
        std::vector<std::string> m_projectTitles; // RequestProjectExport, taken at the first Arm
        std::wstring m_logPath; // the export log (a project's pieces share the whole's)
    };
}
