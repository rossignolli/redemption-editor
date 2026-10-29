#pragma once

// Render export: the encoder behind a bounded queue. Two backends share the
// queue and its writer thread:
//   - LibavSink (libav_encoder.h, the default): libavcodec and libavformat
//     in process, like EVER (FFmpegEncoder.cpp), from the DLLs shipped in
//     RedemptionEditor\dlls. Nothing to install.
//   - FfmpegPipeSink (this file): an ffmpeg.exe child process fed with raw
//     frames on stdin; optional fallback ([Export] Backend=ffmpeg and
//     Ffmpeg=<path of ffmpeg.exe>).
//
// No stutter by construction: the render thread only queues a frame it
// already has in CPU memory; when the queue is full Push waits (the present
// is held, the game waits, and with the export clock nothing in the video
// changes). The writer thread does the blocking work (pipe writes, or the
// filter chain, the encode and the mux). EVER does the same with an 8 frame
// deque and one worker (EncoderSession.cpp).
//
// Frames come from a pool (no allocation per frame at 4K).

#include "export_types.h"

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace render_export
{
    class EncoderSink
    {
    public:
        explicit EncoderSink(std::uint32_t queueCapacity) : m_capacity(queueCapacity ? queueCapacity : 1) {}
        virtual ~EncoderSink() = default;
        EncoderSink(const EncoderSink&) = delete;
        EncoderSink& operator=(const EncoderSink&) = delete;

        virtual bool Open(const EncodeJob& job) = 0;
        // Takes the frame's buffer (swapped out); waits while the queue is
        // full. Returns false when the encoder died (the export fails).
        bool Push(VideoFrame& frame, std::uint32_t& waitedUs);
        // Drains the queue, then closes the backend (trailer, process exit).
        // Runs on a worker thread, never on the render thread. Idempotent.
        bool Close();
        std::uint64_t FramesWritten() const { return m_written.load(); }
        std::uint64_t BytesWritten() const { return m_bytes.load(); }
        std::uint32_t QueueDepth() const;
        ExportError Error() const { return static_cast<ExportError>(m_error.load()); }
        // A buffer from the pool, sized for one frame.
        std::vector<std::uint8_t> TakeBuffer(std::size_t bytes);
        // What encodes (UTF-8, for the log and the dialog): "h264_nvenc", "ffmpeg.exe".
        virtual std::string EncoderName() const = 0;

    protected:
        void StartWriter();
        // Writer thread. The backend may move frame.data out (it then gives
        // it back with Recycle). False fails the export.
        virtual bool BeginBackend() { return true; }
        virtual bool Consume(VideoFrame& frame) = 0;
        virtual bool CloseBackend() = 0;
        void Recycle(std::vector<std::uint8_t>&& buffer);
        void Fail(ExportError error);
        const EncodeJob& Job() const { return m_job; }

        EncodeJob m_job;

    private:
        void WriterLoop();

        const std::uint32_t m_capacity;
        std::thread m_writer;
        mutable std::mutex m_mutex;
        std::condition_variable m_notFull;
        std::condition_variable m_notEmpty;
        std::deque<VideoFrame> m_queue;
        std::vector<std::vector<std::uint8_t>> m_pool;
        bool m_closing = false;
        bool m_closed = false;
        bool m_closeResult = true;
        std::atomic<std::uint64_t> m_written{0};
        std::atomic<std::uint64_t> m_bytes{0};
        std::atomic<std::uint32_t> m_error{0};
    };

    class FfmpegPipeSink final : public EncoderSink
    {
    public:
        explicit FfmpegPipeSink(std::uint32_t queueCapacity = 8) : EncoderSink(queueCapacity) {}
        ~FfmpegPipeSink() override;

        bool Open(const EncodeJob& job) override;
        std::string EncoderName() const override { return "ffmpeg.exe"; }

    private:
        bool Consume(VideoFrame& frame) override;
        bool CloseBackend() override;

        HANDLE m_process = nullptr;
        HANDLE m_stdinWrite = nullptr;
    };

    // Runs ffmpeg once (finalize remux, thumbnail) with its output appended
    // to `logPath`; the exit code, -1 when it did not start, -2 on timeout.
    int RunFfmpeg(const std::wstring& ffmpeg, const std::wstring& args, const std::wstring& logPath, DWORD timeoutMs);
}
