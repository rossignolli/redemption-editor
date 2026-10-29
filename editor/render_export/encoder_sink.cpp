// Render export: the encoder queue and its writer thread (encoder_sink.h),
// the ffmpeg.exe pipe backend, and a helper that runs ffmpeg.exe once
// (finalize, thumbnail) for that backend. No ReShade, no FFmpeg libraries.

#include "encoder_sink.h"

#include "ffmpeg_args.h"
#include "thread_names.h"

#include <algorithm>
#include <chrono>

namespace render_export
{
    // ------------------------------------------------------------------
    // EncoderSink: queue, pool, writer thread
    // ------------------------------------------------------------------

    void EncoderSink::StartWriter()
    {
        m_closing = false;
        m_closed = false;
        m_writer = std::thread([this] {
            replay::threads::Scope threadName("export_encoder");
            WriterLoop();
        });
    }

    std::vector<std::uint8_t> EncoderSink::TakeBuffer(std::size_t bytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::uint8_t> buffer;
        if (!m_pool.empty())
        {
            buffer.swap(m_pool.back());
            m_pool.pop_back();
        }
        buffer.resize(bytes);
        return buffer;
    }

    void EncoderSink::Recycle(std::vector<std::uint8_t>&& buffer)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pool.emplace_back(std::move(buffer));
    }

    bool EncoderSink::Push(VideoFrame& frame, std::uint32_t& waitedUs)
    {
        const auto begin = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(m_mutex);
        // Backpressure: wait while full (the present is held; the export
        // clock makes the wait invisible in the video).
        m_notFull.wait(lock, [this] { return m_queue.size() < m_capacity || m_error.load() != 0; });
        waitedUs = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());
        if (m_error.load() != 0)
            return false;
        m_queue.emplace_back();
        VideoFrame& queued = m_queue.back();
        queued.index = frame.index;
        queued.width = frame.width;
        queued.height = frame.height;
        queued.pixels = frame.pixels;
        queued.data.swap(frame.data);
        m_notEmpty.notify_one();
        return true;
    }

    std::uint32_t EncoderSink::QueueDepth() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<std::uint32_t>(m_queue.size());
    }

    void EncoderSink::WriterLoop()
    {
        if (!BeginBackend())
        {
            Fail(ExportError::EncoderStart);
            return;
        }
        for (;;)
        {
            VideoFrame frame;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_notEmpty.wait(lock, [this] { return !m_queue.empty() || m_closing; });
                if (m_queue.empty())
                    return; // closing and drained
                VideoFrame& front = m_queue.front();
                frame.index = front.index;
                frame.width = front.width;
                frame.height = front.height;
                frame.pixels = front.pixels;
                frame.data.swap(front.data);
                m_queue.pop_front();
                m_notFull.notify_one();
            }
            const std::size_t bytes = frame.data.size();
            if (!Consume(frame))
            {
                if (m_error.load() == 0)
                    Fail(ExportError::EncoderWrite);
                return;
            }
            ++m_written;
            m_bytes += bytes;
            if (frame.data.capacity() != 0)
                Recycle(std::move(frame.data));
        }
    }

    bool EncoderSink::Close()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed)
                return m_closeResult;
            m_closing = true;
        }
        m_notEmpty.notify_all();
        if (m_writer.joinable())
            m_writer.join();
        bool ok = m_error.load() == 0;
        if (!CloseBackend())
        {
            if (m_error.load() == 0)
                Fail(ExportError::EncoderExit);
            ok = false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_closeResult = ok;
        return ok;
    }

    void EncoderSink::Fail(ExportError error)
    {
        std::uint32_t expected = 0;
        m_error.compare_exchange_strong(expected, static_cast<std::uint32_t>(error));
        m_notFull.notify_all();
    }

    // ------------------------------------------------------------------
    // FfmpegPipeSink (optional fallback: ffmpeg.exe)
    // ------------------------------------------------------------------

    FfmpegPipeSink::~FfmpegPipeSink()
    {
        Close();
    }

    bool FfmpegPipeSink::Open(const EncodeJob& job)
    {
        m_job = job;
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE readEnd = nullptr;
        if (!CreatePipe(&readEnd, &m_stdinWrite, &inherit, 8u << 20))
        {
            Fail(ExportError::EncoderStart);
            return false;
        }
        SetHandleInformation(m_stdinWrite, HANDLE_FLAG_INHERIT, 0);
        const std::wstring logPath = job.output + L".ffmpeg.log";
        HANDLE logFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = readEnd;
        startup.hStdOutput = logFile != INVALID_HANDLE_VALUE ? logFile : nullptr;
        startup.hStdError = logFile != INVALID_HANDLE_VALUE ? logFile : nullptr;
        std::wstring commandLine = Quote(job.ffmpeg) + L" " + BuildFfmpegArgs(job);
        PROCESS_INFORMATION info{};
        const BOOL started = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup, &info);
        CloseHandle(readEnd);
        if (logFile != INVALID_HANDLE_VALUE)
            CloseHandle(logFile);
        if (!started)
        {
            CloseHandle(m_stdinWrite);
            m_stdinWrite = nullptr;
            Fail(ExportError::EncoderStart);
            return false;
        }
        CloseHandle(info.hThread);
        m_process = info.hProcess;
        StartWriter();
        return true;
    }

    bool FfmpegPipeSink::Consume(VideoFrame& frame)
    {
        const std::uint8_t* data = frame.data.data();
        std::size_t left = frame.data.size();
        while (left > 0)
        {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(left, 1u << 24));
            if (!WriteFile(m_stdinWrite, data, chunk, &written, nullptr) || written == 0)
            {
                Fail(ExportError::EncoderWrite);
                return false;
            }
            data += written;
            left -= written;
        }
        return true;
    }

    bool FfmpegPipeSink::CloseBackend()
    {
        if (m_stdinWrite)
        {
            CloseHandle(m_stdinWrite); // EOF: ffmpeg writes the trailer
            m_stdinWrite = nullptr;
        }
        bool ok = true;
        if (m_process)
        {
            // The trailer of a long FFV1 file can take a while; this runs on
            // a worker thread (the controller never closes on the render thread).
            WaitForSingleObject(m_process, 120000);
            DWORD code = 1;
            GetExitCodeProcess(m_process, &code);
            ok = code == 0;
            CloseHandle(m_process);
            m_process = nullptr;
        }
        return ok;
    }

    int RunFfmpeg(const std::wstring& ffmpeg, const std::wstring& args, const std::wstring& logPath, DWORD timeoutMs)
    {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE logFile = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nullptr;
        startup.hStdOutput = logFile != INVALID_HANDLE_VALUE ? logFile : nullptr;
        startup.hStdError = logFile != INVALID_HANDLE_VALUE ? logFile : nullptr;
        std::wstring commandLine = Quote(ffmpeg) + L" " + args;
        PROCESS_INFORMATION info{};
        const BOOL started = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
            nullptr, nullptr, &startup, &info);
        if (logFile != INVALID_HANDLE_VALUE)
            CloseHandle(logFile);
        if (!started)
            return -1;
        CloseHandle(info.hThread);
        DWORD code = 1;
        if (WaitForSingleObject(info.hProcess, timeoutMs) != WAIT_OBJECT_0)
        {
            TerminateProcess(info.hProcess, 1);
            code = static_cast<DWORD>(-2);
        }
        else
        {
            GetExitCodeProcess(info.hProcess, &code);
        }
        CloseHandle(info.hProcess);
        return static_cast<int>(code);
    }
}
