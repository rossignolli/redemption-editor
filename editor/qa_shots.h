#pragma once

// QA kit, the editor's side of "mark problem" (Shift+F2, the core's key): the
// screenshot ring, the full picture at the press and the note box. Render
// thread only (the overlay callback), plus worker threads that convert and
// write pictures.
//
// The ring: while the core says a clip plays (qa_shared.h ringAllowed: not
// exporting, not recording, never in gameplay), every 0.5 s the back buffer as
// the game drew it (before the editor's UI) is reduced to 480 rows and kept
// for the last 4.5 s. Reduction on the GPU when the device can blit (Vulkan:
// vkCmdBlitImage into a small image, then a 1.6 MB copy to a host buffer);
// without blit (D3D12) the whole back buffer is copied and reduced on a
// worker. The copy is mapped three frames later (no wait on the GPU) and
// converted on a worker. A JPEG is written only for a mark.
//
// A mark (the core bumps markSequence): the next frame's back buffer at full
// size goes to full.jpg; 2 s after the press the ring's pictures of the 4 s
// before and the 2 s after go to ring-NN_<offset>.jpg in the mark's folder,
// and the add-on reports them (shotsSequence). The note box opens at once over
// the playback (which goes on); Enter sends the note (empty is fine), Esc
// sends none. QA_HEALTH_SUMMARY in the core's log reports the cost.

#include <imgui.h>
#include <reshade.hpp>

#include <Windows.h>

#include "qa_image.h"
#include "qa_jpeg.h"
#include "qa_shared.h"
#include "readback_release.h"
#include "thread_names.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace qa_shots
{
    constexpr ULONGLONG kEveryMs = 500;
    constexpr ULONGLONG kKeepMs = 4500;
    constexpr ULONGLONG kBeforeMs = 4000;
    constexpr ULONGLONG kAfterMs = 2000;
    constexpr std::uint32_t kRingRows = 480;
    constexpr int kMapAfterFrames = 3;

    using replay::qa::image::Pixel;

    struct Frame
    {
        ULONGLONG at = 0;
        std::uint32_t width = 0, height = 0;
        std::vector<std::uint8_t> bgr;
    };

    // One GPU to host copy in flight (the ring's or the full picture's).
    struct Readback
    {
        reshade::api::resource buffer{0};
        std::uint64_t size = 0;
        std::uint32_t width = 0, height = 0; // of the copied image
        std::size_t pitch = 0;
        Pixel kind = Pixel::Bgra8;
        int framesUntilMap = -1;
        bool mapped = false;
        ULONGLONG takenAt = 0;
        std::atomic<bool> working{false};
    };

    struct PendingMark
    {
        bool active = false;
        std::uint32_t id = 0;
        std::wstring folder;
        ULONGLONG pressAt = 0;
        bool fullRequested = false;
        bool fullTaken = false;      // the copy was issued
        std::atomic<int> fullResult{0}; // 0 pending, 1 written, -1 failed
        std::string fullError;
        bool ringWritten = false;
    };

    struct State
    {
        replay::qa::Shared* shared = nullptr;
        unsigned retry = 0;
        reshade::api::device* device = nullptr;

        // Ring.
        std::mutex lock;
        std::deque<Frame> ring; // under lock
        ULONGLONG nextAt = 0;
        reshade::api::resource smallImage{0};
        std::uint32_t smallWidth = 0, smallHeight = 0;
        reshade::api::format smallFormat = reshade::api::format::unknown;
        bool blitBroken = false;
        Readback ringRead;
        int path = 0; // 1 blit, 2 full copy

        // Full picture of a mark.
        Readback fullRead;
        PendingMark mark;
        LONG64 seenMark = 0;
        std::atomic<int> writing{0};

        // Note box.
        bool noteOpen = false;
        bool noteFocus = false;
        std::uint32_t noteMarkId = 0;
        std::uint32_t notePlayheadMs = 0;
        char note[replay::qa::kNoteCapacity] = {};

        // Cost.
        LARGE_INTEGER lastFrame{};
        LARGE_INTEGER frequency{};
        int framesSinceCapture = 100;
        std::uint64_t frames = 0, frameUs = 0;
        std::uint64_t afterFrames = 0, afterUs = 0, afterMax = 0;
        std::uint64_t captures = 0, issueUs = 0, issueMax = 0;
        std::atomic<std::uint64_t> workerUs{0}, workerCount{0};
        bool wasAllowed = false;
    };
    inline State g_state;

    inline std::wstring Widen(const char* text)
    {
        const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        std::wstring out(size > 0 ? static_cast<std::size_t>(size - 1) : 0, L'\0');
        if (size > 1)
            MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
        return out;
    }

    inline bool KindOf(reshade::api::format format, Pixel& kind)
    {
        using namespace reshade::api;
        format = format_to_typeless(format);
        if (format == format::r8g8b8a8_typeless)
            kind = Pixel::Rgba8;
        else if (format == format::b8g8r8a8_typeless || format == format::b8g8r8x8_typeless)
            kind = Pixel::Bgra8;
        else if (format == format::r10g10b10a2_typeless)
            kind = Pixel::Rgb10;
        else if (format == format::r16g16b16a16_typeless)
            kind = Pixel::Half;
        else
            return false;
        return true;
    }

    inline double Us(LARGE_INTEGER a, LARGE_INTEGER b)
    {
        return static_cast<double>(b.QuadPart - a.QuadPart) * 1e6 / static_cast<double>(g_state.frequency.QuadPart);
    }

    inline void DestroyReadback(Readback& read)
    {
        State& s = g_state;
        if (read.buffer.handle && s.device)
        {
            if (read.mapped)
                s.device->unmap_buffer_region(read.buffer);
            s.device->destroy_resource(read.buffer);
        }
        read.buffer = {0};
        read.size = 0;
        read.mapped = false;
        read.framesUntilMap = -1;
    }

    // Release's end of a readback: freed when its worker is done, leaked
    // (and logged) while the worker still reads the mapping.
    inline void ReleaseReadback(Readback& read, const char* which)
    {
        State& s = g_state;
        const editor_readback::Disposal disposal = editor_readback::Dispose(read.buffer.handle != 0 && s.device, read.mapped, read.working.load());
        if (disposal.unmap)
            s.device->unmap_buffer_region(read.buffer);
        if (disposal.destroy)
            s.device->destroy_resource(read.buffer);
        if (disposal.leak)
        {
            char line[128];
            std::snprintf(line, sizeof(line), "QA_SHOTS,status=readback_leaked,buffer=%s,reason=worker_still_running", which);
            reshade::log::message(reshade::log::level::warning, line);
        }
        read.buffer = {0};
        read.size = 0;
        read.mapped = false;
        read.framesUntilMap = -1;
    }

    inline bool EnsureBuffer(reshade::api::device* dev, Readback& read, std::uint64_t size)
    {
        using namespace reshade::api;
        if (read.buffer.handle && read.size == size)
            return true;
        DestroyReadback(read);
        if (!dev->create_resource(resource_desc(size, memory_heap::gpu_to_cpu, resource_usage::copy_dest), nullptr, resource_usage::copy_dest,
                &read.buffer))
        {
            read.buffer = {0};
            return false;
        }
        read.size = size;
        return true;
    }

    // Copies `image` (in copy_source state) into `read`'s host buffer.
    inline bool IssueCopy(reshade::api::command_list* cmd, reshade::api::device* dev, reshade::api::resource image, std::uint32_t width,
        std::uint32_t height, Pixel kind, Readback& read)
    {
        const std::size_t bpp = replay::qa::image::BytesPerPixel(kind);
        const std::size_t pitch = (static_cast<std::size_t>(width) * bpp + 255) & ~static_cast<std::size_t>(255);
        if (!EnsureBuffer(dev, read, static_cast<std::uint64_t>(pitch) * height))
            return false;
        cmd->copy_texture_to_buffer(image, 0, nullptr, read.buffer, 0, static_cast<std::uint32_t>(pitch / bpp), height);
        read.width = width;
        read.height = height;
        read.pitch = pitch;
        read.kind = kind;
        read.framesUntilMap = kMapAfterFrames;
        read.takenAt = GetTickCount64();
        return true;
    }

    inline void PushFrame(Frame&& frame)
    {
        std::lock_guard<std::mutex> guard(g_state.lock);
        g_state.ring.push_back(std::move(frame));
        while (!g_state.ring.empty() && g_state.ring.back().at - g_state.ring.front().at > kKeepMs)
            g_state.ring.pop_front();
        while (g_state.ring.size() > 16)
            g_state.ring.pop_front();
    }

    // The ring's capture of this frame (render thread).
    inline void CaptureRing(reshade::api::effect_runtime* runtime)
    {
        using namespace reshade::api;
        State& s = g_state;
        device* dev = runtime->get_device();
        LARGE_INTEGER a{}, b{};
        QueryPerformanceCounter(&a);
        const resource back = runtime->get_current_back_buffer();
        const resource_desc desc = dev->get_resource_desc(back);
        Pixel kind = Pixel::Bgra8;
        if (!KindOf(desc.texture.format, kind) || desc.texture.width < 64 || desc.texture.height < 64)
            return;
        std::uint32_t width = 0, height = 0;
        replay::qa::image::RingSize(desc.texture.width, desc.texture.height, kRingRows, width, height);
        command_list* cmd = runtime->get_command_queue()->get_immediate_command_list();
        const bool blit = !s.blitBroken && dev->check_capability(device_caps::blit) &&
            (width != desc.texture.width || height != desc.texture.height);
        bool ok = false;
        if (blit)
        {
            if (!s.smallImage.handle || s.smallWidth != width || s.smallHeight != height || s.smallFormat != desc.texture.format)
            {
                if (s.smallImage.handle)
                    dev->destroy_resource(s.smallImage);
                s.smallImage = {0};
                if (!dev->create_resource(resource_desc(width, height, 1, 1, desc.texture.format, 1, memory_heap::gpu_only,
                                              resource_usage::copy_dest | resource_usage::copy_source),
                        nullptr, resource_usage::copy_dest, &s.smallImage))
                {
                    s.smallImage = {0};
                    s.blitBroken = true;
                    reshade::log::message(reshade::log::level::warning, "QA_SHOTS,status=small_image_failed,path=full_copy");
                    return;
                }
                s.smallWidth = width;
                s.smallHeight = height;
                s.smallFormat = desc.texture.format;
            }
            cmd->barrier(back, resource_usage::present, resource_usage::copy_source);
            cmd->copy_texture_region(back, 0, nullptr, s.smallImage, 0, nullptr, filter_mode::min_mag_mip_linear);
            cmd->barrier(back, resource_usage::copy_source, resource_usage::present);
            cmd->barrier(s.smallImage, resource_usage::copy_dest, resource_usage::copy_source);
            ok = IssueCopy(cmd, dev, s.smallImage, width, height, kind, s.ringRead);
            cmd->barrier(s.smallImage, resource_usage::copy_source, resource_usage::copy_dest);
            s.path = 1;
        }
        else
        {
            cmd->barrier(back, resource_usage::present, resource_usage::copy_source);
            ok = IssueCopy(cmd, dev, back, desc.texture.width, desc.texture.height, kind, s.ringRead);
            cmd->barrier(back, resource_usage::copy_source, resource_usage::present);
            s.path = 2;
        }
        QueryPerformanceCounter(&b);
        if (!ok)
            return;
        const std::uint64_t us = static_cast<std::uint64_t>(Us(a, b));
        ++s.captures;
        s.issueUs += us;
        s.issueMax = us > s.issueMax ? us : s.issueMax;
        s.framesSinceCapture = 0;
    }

    // The full picture of a mark (render thread): the back buffer at its
    // size, written as full.jpg on a worker.
    inline void CaptureFull(reshade::api::effect_runtime* runtime)
    {
        using namespace reshade::api;
        State& s = g_state;
        device* dev = runtime->get_device();
        const resource back = runtime->get_current_back_buffer();
        const resource_desc desc = dev->get_resource_desc(back);
        Pixel kind = Pixel::Bgra8;
        if (!KindOf(desc.texture.format, kind))
        {
            s.mark.fullError = "format";
            s.mark.fullResult.store(-1);
            return;
        }
        command_list* cmd = runtime->get_command_queue()->get_immediate_command_list();
        cmd->barrier(back, resource_usage::present, resource_usage::copy_source);
        const bool ok = IssueCopy(cmd, dev, back, desc.texture.width, desc.texture.height, kind, s.fullRead);
        cmd->barrier(back, resource_usage::copy_source, resource_usage::present);
        if (!ok)
        {
            s.mark.fullError = "buffer";
            s.mark.fullResult.store(-1);
        }
    }

    // A copy that is three frames old: mapped, handed to a worker; the
    // buffer is unmapped on a later frame when the worker is done.
    template <class Work>
    inline void ServiceReadback(Readback& read, Work&& work)
    {
        using namespace reshade::api;
        State& s = g_state;
        if (read.mapped && !read.working.load())
        {
            s.device->unmap_buffer_region(read.buffer);
            read.mapped = false;
        }
        if (read.framesUntilMap > 0 && --read.framesUntilMap == 0)
        {
            read.framesUntilMap = -1;
            void* data = nullptr;
            if (s.device->map_buffer_region(read.buffer, 0, UINT64_MAX, map_access::read_only, &data) && data)
            {
                read.mapped = true;
                read.working.store(true);
                work(static_cast<const std::uint8_t*>(data));
            }
        }
    }

    inline void Publish(std::uint32_t markId, std::uint32_t ring, bool full, const std::string& error)
    {
        State& s = g_state;
        if (!s.shared)
            return;
        s.shared->shotsMarkId = markId;
        s.shared->shotsRing = ring;
        s.shared->shotsFull = full ? 1u : 0u;
        std::snprintf(s.shared->shotsError, sizeof(s.shared->shotsError), "%s", error.c_str());
        InterlockedIncrement64(&s.shared->shotsSequence);
    }

    inline void PublishStats()
    {
        State& s = g_state;
        if (!s.shared)
            return;
        replay::qa::Shared& q = *s.shared;
        q.ringCaptures = static_cast<std::uint32_t>(s.captures);
        q.ringIssueUsAvg = s.captures ? static_cast<std::uint32_t>(s.issueUs / s.captures) : 0;
        q.ringIssueUsMax = static_cast<std::uint32_t>(s.issueMax);
        const std::uint64_t count = s.workerCount.load();
        q.ringWorkerUsAvg = count ? static_cast<std::uint32_t>(s.workerUs.load() / count) : 0;
        q.frameUsAvg = s.frames ? static_cast<std::uint32_t>(s.frameUs / s.frames) : 0;
        q.frameUsAfterAvg = s.afterFrames ? static_cast<std::uint32_t>(s.afterUs / s.afterFrames) : 0;
        q.frameUsAfterMax = static_cast<std::uint32_t>(s.afterMax);
        q.ringPath = static_cast<std::uint32_t>(s.path);
        q.ringWidth = s.smallWidth ? s.smallWidth : s.ringRead.width;
        q.ringHeight = s.smallHeight ? s.smallHeight : s.ringRead.height;
        InterlockedIncrement64(&q.statsSequence);
    }

    inline void ResetStats()
    {
        State& s = g_state;
        s.frames = s.frameUs = s.afterFrames = s.afterUs = s.afterMax = 0;
        s.captures = s.issueUs = s.issueMax = 0;
        s.workerUs.store(0);
        s.workerCount.store(0);
    }

    // 2 s after the press: the ring's pictures of the window to files.
    inline void WriteRing()
    {
        State& s = g_state;
        std::vector<Frame> frames;
        {
            std::lock_guard<std::mutex> guard(s.lock);
            for (const Frame& frame : s.ring)
            {
                if (frame.at + kBeforeMs + 250 >= s.mark.pressAt && frame.at <= s.mark.pressAt + kAfterMs + 250)
                    frames.push_back(frame);
            }
        }
        const std::wstring folder = s.mark.folder;
        const std::uint32_t markId = s.mark.id;
        const ULONGLONG press = s.mark.pressAt;
        const bool full = s.mark.fullResult.load() == 1;
        const std::string fullError = s.mark.fullError;
        s.writing.fetch_add(1);
        // The thread pins this module until it ends (the CRT's thread start);
        // Release waits for it.
        std::thread([frames = std::move(frames), folder, markId, press, full, fullError]() {
            replay::threads::Scope threadName("qa_ring_write");
            std::uint32_t written = 0;
            std::string error = fullError;
            try
            {
                int index = 0;
                for (const Frame& frame : frames)
                {
                    const long long offset = static_cast<long long>(frame.at) - static_cast<long long>(press);
                    wchar_t name[64];
                    std::swprintf(name, 64, L"\\ring-%02d_t%+.1fs.jpg", index++, static_cast<double>(offset) / 1000.0);
                    std::string jpegError;
                    if (replay::qa::jpeg::WriteBgr(folder + name, frame.bgr.data(), frame.width, frame.height, 0.8f, jpegError))
                        ++written;
                    else if (error.empty())
                        error = "ring_" + jpegError;
                }
            }
            catch (...)
            {
                error = "ring_exception";
            }
            Publish(markId, written, full, error);
            g_state.writing.fetch_sub(1);
        }).detach();
    }

    // Each frame, from the overlay callback, before the editor's UI.
    // `playing`: the snapshot says a clip plays (the ring also needs the
    // core's ringAllowed). Returns true while the note box is open.
    inline void OnFrame(reshade::api::effect_runtime* runtime)
    {
        State& s = g_state;
        s.device = runtime->get_device();
        if (!s.frequency.QuadPart)
            QueryPerformanceFrequency(&s.frequency);
        if (!s.shared)
        {
            if ((s.retry++ % 60) != 0)
                return;
            s.shared = replay::qa::OpenShared();
            if (!s.shared)
                return;
            s.seenMark = replay::qa::ReadSequence(&s.shared->markSequence); // marks made before this add-on are not ours
        }
        const ULONGLONG now = GetTickCount64();
        InterlockedExchange64(&s.shared->addonFrameAt, static_cast<LONG64>(now));
        const bool allowed = InterlockedCompareExchange(&s.shared->ringAllowed, 0, 0) != 0;

        // Frame times while the ring is allowed (the cost: frames after a
        // capture against all of them).
        LARGE_INTEGER t{};
        QueryPerformanceCounter(&t);
        if (allowed && s.wasAllowed && s.lastFrame.QuadPart)
        {
            const std::uint64_t us = static_cast<std::uint64_t>(Us(s.lastFrame, t));
            if (us < 1000000)
            {
                ++s.frames;
                s.frameUs += us;
                if (s.framesSinceCapture >= 1 && s.framesSinceCapture <= 3)
                {
                    ++s.afterFrames;
                    s.afterUs += us;
                    s.afterMax = us > s.afterMax ? us : s.afterMax;
                }
            }
        }
        s.lastFrame = t;
        ++s.framesSinceCapture;
        if (allowed && !s.wasAllowed)
        {
            ResetStats();
            std::lock_guard<std::mutex> guard(s.lock);
            s.ring.clear();
        }
        if (!allowed && s.wasAllowed)
            PublishStats();
        else if (allowed && (s.frames % 120) == 0)
            PublishStats();
        s.wasAllowed = allowed;

        // Finished copies.
        ServiceReadback(s.ringRead, [](const std::uint8_t* data) {
            State& st = g_state;
            const std::uint32_t width = st.ringRead.width, height = st.ringRead.height;
            const std::size_t pitch = st.ringRead.pitch;
            const Pixel kind = st.ringRead.kind;
            const ULONGLONG at = st.ringRead.takenAt;
            std::thread([data, width, height, pitch, kind, at]() {
                replay::threads::Scope threadName("qa_ring_reduce");
                LARGE_INTEGER a{}, b{};
                QueryPerformanceCounter(&a);
                try
                {
                    Frame frame;
                    frame.at = at;
                    replay::qa::image::RingSize(width, height, kRingRows, frame.width, frame.height);
                    replay::qa::image::ToBgr(data, pitch, width, height, kind, frame.width, frame.height, frame.bgr);
                    PushFrame(std::move(frame));
                }
                catch (...)
                {
                }
                QueryPerformanceCounter(&b);
                g_state.workerUs.fetch_add(static_cast<std::uint64_t>(Us(a, b)));
                g_state.workerCount.fetch_add(1);
                g_state.ringRead.working.store(false);
            }).detach();
        });
        ServiceReadback(s.fullRead, [](const std::uint8_t* data) {
            State& st = g_state;
            const std::uint32_t width = st.fullRead.width, height = st.fullRead.height;
            const std::size_t pitch = st.fullRead.pitch;
            const Pixel kind = st.fullRead.kind;
            const std::wstring path = st.mark.folder + L"\\full.jpg";
            const std::uint32_t markId = st.mark.id;
            st.writing.fetch_add(1);
            std::thread([data, width, height, pitch, kind, path, markId]() {
                replay::threads::Scope threadName("qa_full_write");
                std::string error;
                bool ok = false;
                try
                {
                    std::vector<std::uint8_t> bgr;
                    replay::qa::image::ToBgr(data, pitch, width, height, kind, width, height, bgr);
                    g_state.fullRead.working.store(false); // the buffer is free once converted
                    ok = replay::qa::jpeg::WriteBgr(path, bgr.data(), width, height, 0.9f, error);
                }
                catch (...)
                {
                    error = "exception";
                }
                g_state.fullRead.working.store(false);
                if (g_state.mark.id == markId) // not a later mark's (one waits at most 8 s for this)
                {
                    if (!ok)
                        g_state.mark.fullError = "full_" + error;
                    g_state.mark.fullResult.store(ok ? 1 : -1);
                }
                g_state.writing.fetch_sub(1);
            }).detach();
        });

        // A new mark from the core.
        const LONG64 markSequence = replay::qa::ReadSequence(&s.shared->markSequence);
        if (markSequence != s.seenMark)
        {
            s.seenMark = markSequence;
            if (!s.mark.active || s.mark.ringWritten)
            {
                s.mark.active = true;
                s.mark.id = s.shared->markId;
                char folder[replay::qa::kFolderCapacity]{};
                std::memcpy(folder, s.shared->markFolder, sizeof(folder) - 1);
                s.mark.folder = Widen(folder);
                s.mark.pressAt = now;
                s.mark.fullRequested = true;
                s.mark.fullTaken = false;
                s.mark.fullResult.store(0);
                s.mark.fullError.clear();
                s.mark.ringWritten = false;
                // The note box (closing a previous one unanswered).
                s.noteOpen = true;
                s.noteFocus = true;
                s.noteMarkId = s.mark.id;
                s.notePlayheadMs = s.shared->markPlayheadMs;
                s.note[0] = '\0';
                InterlockedExchange(&s.shared->noteOpen, 1);
            }
        }
        if (s.mark.active && s.mark.fullRequested && !s.mark.fullTaken && s.fullRead.framesUntilMap < 0 && !s.fullRead.mapped)
        {
            s.mark.fullTaken = true;
            CaptureFull(runtime);
        }
        if (s.mark.active && !s.mark.ringWritten && now >= s.mark.pressAt + kAfterMs + 400 &&
            (s.mark.fullResult.load() != 0 || now >= s.mark.pressAt + 8000))
        {
            s.mark.ringWritten = true;
            WriteRing();
        }

        // The ring.
        if (allowed && now >= s.nextAt && s.ringRead.framesUntilMap < 0 && !s.ringRead.mapped && !s.ringRead.working.load())
        {
            s.nextAt = now + kEveryMs;
            CaptureRing(runtime);
        }
    }

    inline bool NoteOpen() { return g_state.noteOpen; }

    // Shift+F2 while the editor has the keyboard (ScriptHook does not see
    // keys then): the core marks as for its own key.
    inline void RequestMark()
    {
        if (g_state.shared)
            InterlockedIncrement64(&g_state.shared->markRequestSequence);
    }

    inline void RequestReport()
    {
        if (g_state.shared)
            InterlockedIncrement64(&g_state.shared->reportRequestSequence);
    }

    inline void SendNote(bool answered)
    {
        State& s = g_state;
        s.noteOpen = false;
        if (!s.shared)
            return;
        s.shared->noteMarkId = s.noteMarkId;
        s.shared->noteAnswered = answered ? 1u : 0u;
        std::snprintf(s.shared->note, sizeof(s.shared->note), "%s", answered ? s.note : "");
        InterlockedIncrement64(&s.shared->noteSequence);
        InterlockedExchange(&s.shared->noteOpen, 0);
    }

    inline bool MarksEnabled()
    {
        return g_state.shared && InterlockedCompareExchange(&g_state.shared->marksEnabled, 0, 0) != 0;
    }

    // The player's marks of this session not sent yet (the core's count,
    // qa_marks.h CountPendingMarks: an earlier session's folders in
    // ReplayReports\marks go into the next report but are not counted).
    inline std::uint32_t PendingMarks()
    {
        if (!MarksEnabled())
            return 0;
        const LONG pending = InterlockedCompareExchange(&g_state.shared->pendingMarks, 0, 0);
        return pending > 0 ? static_cast<std::uint32_t>(pending) : 0u;
    }

    inline void Release()
    {
        State& s = g_state;
        // The reduce and convert workers read the mapped readback buffers:
        // waited for (bounded, the render thread); a buffer whose worker is
        // still busy is leaked, never freed under it.
        editor_readback::WaitIdle([&s] { return s.ringRead.working.load() || s.fullRead.working.load() || s.writing.load() > 0; }, 2000,
            [] { return static_cast<std::uint64_t>(GetTickCount64()); }, [] { Sleep(1); });
        ReleaseReadback(s.ringRead, "ring");
        ReleaseReadback(s.fullRead, "full");
        if (s.smallImage.handle && s.device)
            s.device->destroy_resource(s.smallImage);
        s.smallImage = {0};
        s.smallWidth = s.smallHeight = 0;
        std::lock_guard<std::mutex> guard(s.lock);
        s.ring.clear();
        if (s.noteOpen)
            s.noteOpen = false;
        s.device = nullptr;
    }
}
