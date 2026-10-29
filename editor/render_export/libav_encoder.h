#pragma once

// Render export: the FFmpeg libraries in process, the way EVER Enhanced
// does it (EVER/src/video/FFmpegEncoder.cpp, EVER/src/core/dllmain.cpp):
//
//   - The DLLs (avcodec, avformat, avfilter, avutil, swscale, swresample and
//     the codec libraries they need: x264, x265, libaom, libvpx, opus, lame,
//     vorbis, libvpl) ship in RedemptionEditor\dlls\ next to RDR2.exe.
//   - The editor core links them with /DELAYLOAD and LoadFfmpegRuntime loads
//     each one first by its full path with LoadLibraryExW(
//     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS):
//     their own imports resolve from that folder, the process search path
//     is never changed (no SetDllDirectory, no PATH), and the delay-load
//     stubs then bind to the modules already in memory. No call reaches a
//     delay-loaded function before that load succeeded.
//   - The libraries stay loaded until the game exits (the hot swappable
//     core is unloaded and loaded again; the DLLs are not).
//
// Frames are in system memory, already in the encoder's layout from the GPU
// (NV12, P010, packed RGB): NVENC, AMF and QSV upload them themselves, like
// EVER. The LUT, the preview scale and the RGB to YUV conversion of the
// masters run in a libavfilter chain (encoder_select.h VideoFilterChain).
//
// Nothing here runs on the render thread except LibavSink::Open (stores the
// job); the encoder opens on the writer thread before the first frame.

#include "encoder_select.h"
#include "encoder_sink.h"
#include "export_types.h"
#include "ffmpeg_args.h"
#include "project_parts.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFilterGraph;
struct AVFilterContext;
struct AVFrame;
struct AVPacket;
struct AVStream;

namespace render_export
{
    // <folder of RDR2.exe>\RedemptionEditor\dlls (the release zip's layout).
    std::wstring DefaultFfmpegDllFolder();

    // Loads the FFmpeg DLLs from `folder` once per process (EVER's
    // LoadLibraryExW loop). False with `error` when a DLL is missing or does
    // not load; nothing of FFmpeg may be called then.
    bool LoadFfmpegRuntime(const std::wstring& folder, std::string& error);
    bool FfmpegRuntimeLoaded();
    // "8.1.2 (avcodec 62.x)": for the log.
    std::string FfmpegVersionText();
    // Before the core unloads: FFmpeg's log callback points into the core.
    void ReleaseFfmpegLog();
    // FFmpeg's warnings and errors are appended to this file (empty: off).
    void SetFfmpegLogFile(const std::wstring& path);

    // Opens every variant of encoder_select.h on a small frame (1280x720)
    // and records which ones work on this PC (the GPU, its driver, the AMD
    // and Intel runtimes). A second or so; run it off the render thread.
    ProbeResult ProbeEncoders();

    // The in process encoder (the default backend).
    class LibavSink final : public EncoderSink
    {
    public:
        explicit LibavSink(std::uint32_t queueCapacity = 8) : EncoderSink(queueCapacity) {}
        ~LibavSink() override;

        // Stores the job and starts the writer thread; the encoder opens
        // there (a failure shows at the first Push).
        bool Open(const EncodeJob& job) override;
        std::string EncoderName() const override;
        // The variant that opened (-1 until it did) and, when none did, why.
        int Variant() const { return m_variant; }
        std::string OpenError() const;

    private:
        bool BeginBackend() override;
        bool Consume(VideoFrame& frame) override;
        bool CloseBackend() override;

        bool OpenVariant(int index, std::string& error);
        bool OpenFilters(const std::string& chain, std::string& error);
        bool Encode(AVFrame* frame);
        bool Drain();
        void Thumbnail(const AVFrame* frame);
        void Free();

        mutable std::mutex m_openMutex;
        bool m_opened = false;
        std::string m_openError;
        std::atomic<int> m_variant{-1};
        std::string m_encoderName;
        std::int64_t m_pts = 0;
        bool m_headerWritten = false;
        bool m_thumbnailDone = false;
        AVFormatContext* m_format = nullptr;
        AVCodecContext* m_codec = nullptr;
        AVStream* m_stream = nullptr;
        AVFilterGraph* m_graph = nullptr;
        AVFilterContext* m_source = nullptr;
        AVFilterContext* m_sink = nullptr;
        AVFrame* m_filtered = nullptr;
        AVPacket* m_packet = nullptr;
    };

    // The finalize without ffmpeg.exe: the video pass's file copied packet
    // by packet into the final file, plus the audio pass WAV (from
    // `audioStartS`, cut at the video's length, encoded AAC in MP4, PCM 24
    // in MOV, FLAC in MKV) and the markers as chapters. `frames` is the
    // video's frame count (the cut of the audio).
    bool LibavFinalize(const EncodeJob& job, const std::wstring& videoPath, double audioStartS, const std::vector<Chapter>& chapters,
        std::uint64_t durationMs, std::uint64_t frames, std::string& error);
    // A whole project: the pieces' video passes copied one after the other
    // (piece k from the frame after piece k-1's last, by their frame counts),
    // all encoded alike (the same settings); `frames` their sum.
    bool LibavFinalizeParts(const EncodeJob& job, const std::vector<VideoPart>& parts, double audioStartS, const std::vector<Chapter>& chapters,
        std::uint64_t durationMs, std::uint64_t frames, std::string& error);
}
