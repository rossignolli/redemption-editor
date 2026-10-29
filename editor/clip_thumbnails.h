#pragma once

// Clip thumbnails for the clip gallery. While a clip records, a frame of the
// back buffer is taken every few seconds and kept small; when the core reports
// the clip saved, the middle frame is written next to it as <clip>.bmp
// (480x270, 24-bit, no encoder needed). The gallery loads it into its own
// texture. Render thread only (present and overlay callbacks).

#include <imgui.h>
#include <reshade.hpp>

#include <Windows.h>
#include "readback_release.h"
#include "storage_paths.h" // ReplayClips of the base folder, the game folder's read too
#include "thread_names.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace clip_thumbnails
{
    constexpr std::uint32_t kWidth = 480;
    constexpr std::uint32_t kHeight = 270;
    constexpr ULONGLONG kFirstFrameMs = 1500;
    constexpr ULONGLONG kFrameEveryMs = 5000;
    constexpr std::size_t kMaxFrames = 16;
    constexpr std::size_t kMaxTextures = 12;

    using Frame = std::vector<std::uint8_t>; // kWidth * kHeight * 3, RGB, top row first

    struct Texture
    {
        reshade::api::resource resource{0};
        reshade::api::resource_view view{0};
        ULONGLONG usedAt = 0;
        bool missing = false; // no file: not retried every frame
    };

    enum class PixelKind { Rgba8, Bgra8, Rgb10, Half };

    struct State
    {
        bool recording = false;
        ULONGLONG nextFrameAt = 0;
        std::vector<Frame> frames; // guarded by framesLock
        std::mutex framesLock;
        std::atomic<bool> downsampling{false};
        // Asynchronous readback: the back buffer is copied into a CPU-readable buffer
        // and mapped a few frames later, when the GPU is done with it (a synchronous
        // capture_screenshot costs about 20 ms on the render thread).
        reshade::api::resource readback{0};
        std::uint64_t readbackSize = 0;
        std::uint32_t rbWidth = 0, rbHeight = 0;
        std::size_t rbPitch = 0;
        PixelKind rbKind{};
        int framesUntilMap = -1;
        bool mapped = false;
        reshade::api::device* device = nullptr;
        std::map<std::string, Texture> textures;
        // A thumbnail file read on a thread (on the render thread, a held arrow key
        // would read one file a frame). One read at a time; the texture is made here
        // when it is done.
        struct Load
        {
            std::string name;
            std::vector<std::uint32_t> rgba;
            bool ok = false;
            bool stale = false; // render thread: the file changed meanwhile (T, a delete)
        };
        std::shared_ptr<Load> load;       // render thread only
        std::atomic<bool> loading{false}; // the thread has not finished it
        // Thumbnail files being written on a thread (no read starts meanwhile).
        std::atomic<int> writing{0};
    };
    inline State g_state;

    // "042-Valentine.bmp" for "042-Valentine.rpclip".
    inline std::wstring ThumbName(const std::string& clipName)
    {
        std::string stem = clipName;
        const std::size_t dot = stem.rfind(".rpclip");
        if (dot != std::string::npos)
            stem.resize(dot);
        return std::wstring(stem.begin(), stem.end()) + L".bmp";
    }

    // Written to ReplayClips of the base folder (storage_paths.h: the game
    // folder, else Documents), whichever folder holds the clip.
    inline std::wstring ThumbPath(const std::string& clipName)
    {
        return replay::storage::Folder(replay::storage::kClips) + L"\\" + ThumbName(clipName);
    }

    // Read from the base folder, else next to a clip of the game folder.
    inline std::wstring ThumbReadPath(const std::string& clipName)
    {
        return replay::storage::Find(std::wstring(replay::storage::kClips) + L"\\" + ThumbName(clipName));
    }

    inline float HalfToFloat(std::uint16_t h)
    {
        const std::uint32_t sign = (h & 0x8000u) << 16;
        std::uint32_t exponent = (h >> 10) & 0x1F;
        std::uint32_t mantissa = h & 0x3FF;
        std::uint32_t bits = 0;
        if (exponent == 0)
            bits = sign; // zero and subnormals (dark enough to be black here)
        else if (exponent == 31)
            bits = sign | 0x7F800000u | (mantissa << 13);
        else
            bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    inline std::uint8_t ToByte(float value)
    {
        value = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<std::uint8_t>(value * 255.0f + 0.5f);
    }

    // Box-samples a back buffer copy down to kWidth x kHeight RGB. Pure CPU; runs
    // on a worker thread (most of the capture's cost).
    inline void Downsample(const std::uint8_t* raw, std::size_t rowPitch, std::uint32_t width, std::uint32_t height,
        PixelKind kind, Frame& out)
    {
        const bool half = kind == PixelKind::Half;
        const bool rgb10 = kind == PixelKind::Rgb10;
        const bool bgra8 = kind == PixelKind::Bgra8;
        const std::size_t bpp = half ? 8 : 4;
        out.assign(static_cast<std::size_t>(kWidth) * kHeight * 3, 0);
        constexpr int kTaps = 3; // 3x3 samples per output pixel
        for (std::uint32_t y = 0; y < kHeight; ++y)
        {
            for (std::uint32_t x = 0; x < kWidth; ++x)
            {
                float sum[3] = {};
                for (int ty = 0; ty < kTaps; ++ty)
                {
                    for (int tx = 0; tx < kTaps; ++tx)
                    {
                        const std::size_t sx = (static_cast<std::size_t>(x) * kTaps + tx) * width / (kWidth * kTaps);
                        const std::size_t sy = (static_cast<std::size_t>(y) * kTaps + ty) * height / (kHeight * kTaps);
                        const std::uint8_t* p = raw + sy * rowPitch + sx * bpp;
                        float rgb[3];
                        if (half)
                        {
                            std::uint16_t h[3];
                            std::memcpy(h, p, sizeof(h));
                            for (int c = 0; c < 3; ++c)
                                rgb[c] = HalfToFloat(h[c]);
                        }
                        else if (rgb10)
                        {
                            std::uint32_t v = 0;
                            std::memcpy(&v, p, 4);
                            rgb[0] = static_cast<float>(v & 0x3FF) / 1023.0f;
                            rgb[1] = static_cast<float>((v >> 10) & 0x3FF) / 1023.0f;
                            rgb[2] = static_cast<float>((v >> 20) & 0x3FF) / 1023.0f;
                        }
                        else
                        {
                            rgb[0] = p[bgra8 ? 2 : 0] / 255.0f;
                            rgb[1] = p[1] / 255.0f;
                            rgb[2] = p[bgra8 ? 0 : 2] / 255.0f;
                        }
                        for (int c = 0; c < 3; ++c)
                            sum[c] += rgb[c];
                    }
                }
                std::uint8_t* d = out.data() + (static_cast<std::size_t>(y) * kWidth + x) * 3;
                for (int c = 0; c < 3; ++c)
                    d[c] = ToByte(sum[c] / (kTaps * kTaps));
            }
        }
    }

    // The back buffer copy (render thread): the part that has to wait for
    // the GPU.
    inline bool CaptureRaw(reshade::api::effect_runtime* runtime, std::vector<std::uint8_t>& raw,
        std::uint32_t& width, std::uint32_t& height, PixelKind& kind)
    {
        using namespace reshade::api;
        runtime->get_screenshot_width_and_height(&width, &height);
        if (width < kWidth || height < kHeight)
            return false;
        const format fmt = format_to_typeless(runtime->get_device()->get_resource_desc(runtime->get_current_back_buffer()).texture.format);
        if (fmt == format::r8g8b8a8_typeless)
            kind = PixelKind::Rgba8;
        else if (fmt == format::b8g8r8a8_typeless || fmt == format::b8g8r8x8_typeless)
            kind = PixelKind::Bgra8;
        else if (fmt == format::r10g10b10a2_typeless)
            kind = PixelKind::Rgb10;
        else if (fmt == format::r16g16b16a16_typeless)
            kind = PixelKind::Half;
        else
            return false;
        raw.resize(static_cast<std::size_t>(width) * height * (kind == PixelKind::Half ? 8 : 4));
        return runtime->capture_screenshot(raw.data());
    }

    inline void PushFrame(Frame&& frame)
    {
        State& st = g_state;
        std::lock_guard<std::mutex> lock(st.framesLock);
        if (st.frames.size() >= kMaxFrames)
            st.frames.erase(st.frames.begin());
        st.frames.push_back(std::move(frame));
    }

    inline bool KindOf(reshade::api::format fmt, PixelKind& kind)
    {
        using namespace reshade::api;
        fmt = format_to_typeless(fmt);
        if (fmt == format::r8g8b8a8_typeless)
            kind = PixelKind::Rgba8;
        else if (fmt == format::b8g8r8a8_typeless || fmt == format::b8g8r8x8_typeless)
            kind = PixelKind::Bgra8;
        else if (fmt == format::r10g10b10a2_typeless)
            kind = PixelKind::Rgb10;
        else if (fmt == format::r16g16b16a16_typeless)
            kind = PixelKind::Half;
        else
            return false;
        return true;
    }

    // Present callback: frames while the core records, a ring of the last
    // kMaxFrames (the always-on replay buffer records for hours; its saved clip is
    // its last seconds).
    inline void OnPresent(reshade::api::effect_runtime* runtime, bool recording, bool paused)
    {
        using namespace reshade::api;
        State& s = g_state;
        device* dev = runtime->get_device();
        s.device = dev;
        const ULONGLONG now = GetTickCount64();
        // A reduced frame is done: the buffer can be read again.
        if (s.mapped && !s.downsampling.load())
        {
            dev->unmap_buffer_region(s.readback);
            s.mapped = false;
        }
        // The copy issued a few frames ago is ready: map it and reduce it on
        // a thread straight from the mapped memory.
        if (s.framesUntilMap > 0 && --s.framesUntilMap == 0)
        {
            s.framesUntilMap = -1;
            void* data = nullptr;
            if (dev->map_buffer_region(s.readback, 0, UINT64_MAX, map_access::read_only, &data) && data)
            {
                s.mapped = true;
                s.downsampling.store(true);
                const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
                const std::size_t pitch = s.rbPitch;
                const std::uint32_t width = s.rbWidth, height = s.rbHeight;
                const PixelKind kind = s.rbKind;
                // The thread pins this module until it ends (the CRT's
                // thread start), so a hot swap of the editor core waits.
                std::thread([bytes, pitch, width, height, kind]() {
                    replay::threads::Scope threadName("thumb_downsample");
                    // An exception leaving the thread is std::terminate (the game closes); the
                    // frame is lost.
                    try
                    {
                        Frame frame;
                        Downsample(bytes, pitch, width, height, kind, frame);
                        PushFrame(std::move(frame));
                    }
                    catch (...)
                    {
                    }
                    g_state.downsampling.store(false);
                }).detach();
            }
        }
        if (recording && !s.recording)
        {
            std::lock_guard<std::mutex> lock(s.framesLock);
            s.frames.clear();
            s.nextFrameAt = now + kFirstFrameMs;
        }
        s.recording = recording;
        if (!recording || paused || now < s.nextFrameAt || s.downsampling.load() || s.mapped || s.framesUntilMap > 0)
            return;
        s.nextFrameAt = now + kFrameEveryMs;

        LARGE_INTEGER a{}, b{}, f{};
        QueryPerformanceCounter(&a);
        const resource back = runtime->get_current_back_buffer();
        const resource_desc desc = dev->get_resource_desc(back);
        PixelKind kind = PixelKind::Rgba8;
        const std::uint32_t width = desc.texture.width, height = desc.texture.height;
        if (width < kWidth || height < kHeight || !KindOf(desc.texture.format, kind))
            return;
        const std::size_t bpp = kind == PixelKind::Half ? 8 : 4;
        const std::size_t pitch = (static_cast<std::size_t>(width) * bpp + 255) & ~static_cast<std::size_t>(255);
        const std::uint64_t size = static_cast<std::uint64_t>(pitch) * height;
        if (!s.readback.handle || s.readbackSize != size)
        {
            if (s.readback.handle)
                dev->destroy_resource(s.readback);
            s.readback = {0};
            s.readbackSize = 0;
            if (!dev->create_resource(resource_desc(size, memory_heap::gpu_to_cpu, resource_usage::copy_dest), nullptr,
                    resource_usage::copy_dest, &s.readback))
            {
                s.readback = {0};
                reshade::log::message(reshade::log::level::warning, "THUMB_READBACK,status=create_failed");
                return;
            }
            s.readbackSize = size;
        }
        command_list* cmd = runtime->get_command_queue()->get_immediate_command_list();
        cmd->barrier(back, resource_usage::present, resource_usage::copy_source);
        cmd->copy_texture_to_buffer(back, 0, nullptr, s.readback, 0, static_cast<std::uint32_t>(pitch / bpp), height);
        cmd->barrier(back, resource_usage::copy_source, resource_usage::present);
        s.rbWidth = width;
        s.rbHeight = height;
        s.rbPitch = pitch;
        s.rbKind = kind;
        s.framesUntilMap = 3;
        QueryPerformanceCounter(&b);
        QueryPerformanceFrequency(&f);
        char line[96];
        std::snprintf(line, sizeof(line), "THUMB_TIMING,issue_ms=%.2f",
            static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart));
        reshade::log::message(reshade::log::level::info, line);
    }

    inline bool WriteBmp(const std::wstring& path, const Frame& frame)
    {
        const std::uint32_t rowBytes = kWidth * 3; // 1440, already a multiple of 4
        const std::uint32_t imageBytes = rowBytes * kHeight;
        std::uint8_t header[54]{};
        header[0] = 'B';
        header[1] = 'M';
        const std::uint32_t fileSize = 54 + imageBytes;
        std::memcpy(header + 2, &fileSize, 4);
        const std::uint32_t offset = 54, infoSize = 40;
        std::memcpy(header + 10, &offset, 4);
        std::memcpy(header + 14, &infoSize, 4);
        const std::int32_t w = kWidth, h = -static_cast<std::int32_t>(kHeight); // top-down rows
        std::memcpy(header + 18, &w, 4);
        std::memcpy(header + 22, &h, 4);
        const std::uint16_t planes = 1, bits = 24;
        std::memcpy(header + 26, &planes, 2);
        std::memcpy(header + 28, &bits, 2);
        std::memcpy(header + 34, &imageBytes, 4);
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
            return false;
        std::fwrite(header, 1, sizeof(header), file);
        std::vector<std::uint8_t> row(rowBytes);
        for (std::uint32_t y = 0; y < kHeight; ++y)
        {
            const std::uint8_t* src = frame.data() + static_cast<std::size_t>(y) * rowBytes;
            for (std::uint32_t x = 0; x < kWidth; ++x)
            {
                row[x * 3 + 0] = src[x * 3 + 2]; // BMP stores BGR
                row[x * 3 + 1] = src[x * 3 + 1];
                row[x * 3 + 2] = src[x * 3 + 0];
            }
            std::fwrite(row.data(), 1, rowBytes, file);
        }
        const bool ok = std::ferror(file) == 0;
        std::fclose(file);
        return ok;
    }

    inline void Forget(const std::string& clipName);

    // The core saved a new recording: its thumbnail is the middle frame taken. The
    // file (230 KB) is written on a thread, not in the overlay callback; the frame
    // goes with it and no thumbnail is read until it is written.
    inline void OnClipSaved(const std::string& clipName)
    {
        State& s = g_state;
        Frame frame;
        {
            std::lock_guard<std::mutex> lock(s.framesLock);
            if (!s.frames.empty())
                frame = std::move(s.frames[s.frames.size() / 2]);
            s.frames.clear();
        }
        Forget(clipName); // a new file: read it next time
        if (frame.empty())
            return;
        s.writing.fetch_add(1);
        // The thread pins this module until it ends (the CRT's thread
        // start); Release waits for it.
        std::thread([frame = std::move(frame), path = ThumbPath(clipName)]() {
            replay::threads::Scope threadName("thumb_write");
            try // no exception may leave the thread
            {
                WriteBmp(path, frame);
            }
            catch (...)
            {
            }
            g_state.writing.fetch_sub(1);
        }).detach();
    }

    inline bool ReadBmp(const std::wstring& path, std::vector<std::uint32_t>& rgba)
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
            return false;
        std::uint8_t header[54]{};
        bool ok = std::fread(header, 1, sizeof(header), file) == sizeof(header) && header[0] == 'B' && header[1] == 'M';
        std::int32_t w = 0, h = 0;
        std::uint16_t bits = 0;
        std::uint32_t offset = 0;
        std::memcpy(&offset, header + 10, 4);
        std::memcpy(&w, header + 18, 4);
        std::memcpy(&h, header + 22, 4);
        std::memcpy(&bits, header + 28, 2);
        ok = ok && w == static_cast<std::int32_t>(kWidth) && (h == static_cast<std::int32_t>(kHeight) || h == -static_cast<std::int32_t>(kHeight)) && bits == 24;
        if (ok)
        {
            std::fseek(file, static_cast<long>(offset), SEEK_SET);
            rgba.assign(static_cast<std::size_t>(kWidth) * kHeight, 0);
            std::vector<std::uint8_t> row(kWidth * 3);
            for (std::uint32_t y = 0; ok && y < kHeight; ++y)
            {
                ok = std::fread(row.data(), 1, row.size(), file) == row.size();
                const std::uint32_t destY = h < 0 ? y : kHeight - 1 - y;
                // R8G8B8A8 in memory order (the BMP rows are BGR).
                for (std::uint32_t x = 0; ok && x < kWidth; ++x)
                    rgba[destY * kWidth + x] = 0xFF000000u | (static_cast<std::uint32_t>(row[x * 3 + 0]) << 16) |
                        (static_cast<std::uint32_t>(row[x * 3 + 1]) << 8) | row[x * 3 + 2];
            }
        }
        std::fclose(file);
        return ok;
    }

    inline void DestroyTexture(Texture& texture)
    {
        State& s = g_state;
        if (!s.device)
            return;
        if (texture.view.handle)
            s.device->destroy_resource_view(texture.view);
        if (texture.resource.handle)
            s.device->destroy_resource(texture.resource);
        texture.view = {0};
        texture.resource = {0};
    }

    // A read thumbnail into a texture (render thread).
    inline void MakeTexture(const std::string& clipName, const std::vector<std::uint32_t>& rgba)
    {
        using namespace reshade::api;
        State& s = g_state;
        Texture& texture = s.textures[clipName];
        // Keep a handful of textures: the least recently shown goes.
        if (s.textures.size() > kMaxTextures)
        {
            auto oldest = s.textures.end();
            for (auto it = s.textures.begin(); it != s.textures.end(); ++it)
                if (it->first != clipName && it->second.view.handle && (oldest == s.textures.end() || it->second.usedAt < oldest->second.usedAt))
                    oldest = it;
            if (oldest != s.textures.end())
                DestroyTexture(oldest->second);
        }
        subresource_data data{};
        data.data = const_cast<std::uint32_t*>(rgba.data()); // read only by create_resource
        data.row_pitch = kWidth * 4;
        data.slice_pitch = kWidth * kHeight * 4;
        const resource_desc desc(kWidth, kHeight, 1, 1, format::r8g8b8a8_unorm, 1, memory_heap::gpu_only, resource_usage::shader_resource);
        if (!s.device->create_resource(desc, &data, resource_usage::shader_resource, &texture.resource) ||
            !s.device->create_resource_view(texture.resource, resource_usage::shader_resource, resource_view_desc(format::r8g8b8a8_unorm), &texture.view))
        {
            DestroyTexture(texture);
            texture.missing = true;
        }
    }

    // The clip's thumbnail texture, 0 when it has none or while its file is
    // being read (Loading tells which).
    inline ImTextureID Thumbnail(reshade::api::effect_runtime* runtime, const std::string& clipName)
    {
        State& s = g_state;
        if (!runtime)
            return 0;
        s.device = runtime->get_device();
        // A read that finished becomes its clip's texture.
        if (s.load && !s.loading.load())
        {
            const std::shared_ptr<State::Load> done = std::move(s.load);
            s.load.reset();
            if (!done->stale && done->ok) // a stale one is read again when asked
                MakeTexture(done->name, done->rgba);
            else if (!done->stale)
                s.textures[done->name].missing = true;
        }
        Texture& texture = s.textures[clipName];
        texture.usedAt = GetTickCount64();
        if (texture.view.handle)
            return static_cast<ImTextureID>(texture.view.handle);
        if (texture.missing || s.load || s.writing.load() > 0)
            return 0; // none, or wait for the read (or a write) in flight, then this one
        auto load = std::make_shared<State::Load>();
        load->name = clipName;
        s.load = load;
        s.loading.store(true);
        const std::wstring path = ThumbReadPath(clipName);
        // The thread pins this module until it ends (the CRT's thread start);
        // Release waits for it.
        std::thread([load, path]() {
            replay::threads::Scope threadName("thumb_read");
            try // no exception may leave the thread
            {
                load->ok = ReadBmp(path, load->rgba);
            }
            catch (...)
            {
                load->ok = false;
            }
            g_state.loading.store(false);
        }).detach();
        return 0;
    }

    // True while the clip's thumbnail is being read (the gallery shows "loading"
    // instead of "no thumbnail").
    inline bool Loading(const std::string& clipName)
    {
        const State& s = g_state;
        auto found = s.textures.find(clipName);
        const bool known = found != s.textures.end() && (found->second.view.handle || found->second.missing);
        return !known;
    }

    // A clip was deleted: its texture goes too.
    inline void Forget(const std::string& clipName)
    {
        if (g_state.load && g_state.load->name == clipName)
            g_state.load->stale = true; // the read in flight is of the old file
        auto found = g_state.textures.find(clipName);
        if (found == g_state.textures.end())
            return;
        DestroyTexture(found->second);
        g_state.textures.erase(found);
    }

    // The frame on screen now as the clip's thumbnail (the editor's T): the back
    // buffer before the overlay, reduced and written over <clip>.bmp; the cached
    // texture is dropped so the gallery reloads it.
    inline bool SetFromScreen(reshade::api::effect_runtime* runtime, const std::string& clipName)
    {
        std::vector<std::uint8_t> raw;
        std::uint32_t width = 0, height = 0;
        PixelKind kind = PixelKind::Rgba8;
        if (!CaptureRaw(runtime, raw, width, height, kind))
            return false;
        Frame frame;
        const std::size_t bpp = kind == PixelKind::Half ? 8 : 4;
        Downsample(raw.data(), static_cast<std::size_t>(width) * bpp, width, height, kind, frame);
        if (!WriteBmp(ThumbPath(clipName), frame))
            return false;
        Forget(clipName);
        return true;
    }

    inline void Release()
    {
        for (auto& [name, texture] : g_state.textures)
            DestroyTexture(texture);
        // A frame being reduced on its thread reads the mapped readback buffer:
        // waited for (bounded, the render thread), never freed under it.
        const bool downsampleDone = editor_readback::WaitIdle([] { return g_state.downsampling.load(); }, 1000,
            [] { return static_cast<std::uint64_t>(GetTickCount64()); }, [] { Sleep(1); });
        for (int wait = 0; wait < 1000 && g_state.loading.load(); ++wait)
            Sleep(1); // a thumbnail being read on its thread
        for (int wait = 0; wait < 1000 && g_state.writing.load() > 0; ++wait)
            Sleep(1); // a thumbnail being written on its thread
        g_state.load.reset();
        if (g_state.device)
        {
            const editor_readback::Disposal disposal =
                editor_readback::Dispose(g_state.readback.handle != 0, g_state.mapped, !downsampleDone);
            if (disposal.unmap)
                g_state.device->unmap_buffer_region(g_state.readback);
            if (disposal.destroy)
                g_state.device->destroy_resource(g_state.readback);
            if (disposal.leak)
                reshade::log::message(reshade::log::level::warning,
                    "THUMB_READBACK,status=leaked,reason=downsample_still_running (the buffer stays mapped for its thread)");
        }
        g_state.readback = {0};
        g_state.readbackSize = 0;
        g_state.mapped = false;
        g_state.framesUntilMap = -1;
        std::lock_guard<std::mutex> lock(g_state.framesLock);
        g_state.recording = false;
        g_state.nextFrameAt = 0;
        g_state.frames.clear();
        g_state.device = nullptr;
        g_state.textures.clear();
    }
}
