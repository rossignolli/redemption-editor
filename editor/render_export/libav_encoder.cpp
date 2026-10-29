// Render export: the FFmpeg libraries in process (libav_encoder.h). EVER
// Enhanced's FFmpegEncoder.cpp is the model: encoder by name, options as
// key=value pairs, libavfilter for the conversions, an AVPacket loop into
// libavformat. Differences on purpose: the frames come from the GPU already
// in the encoder's layout (no swscale for NVENC), every YUV stream is tagged
// BT.709 limited (EVER left the matrix untagged), the GPU buffers are handed
// to libavcodec without a copy (AVBufferRef over the pool buffer), and the
// thumbnail is taken from the frame on its way in (no decode afterwards).

#include "libav_encoder.h"
#include "../bundled_files.h"

#pragma warning(push, 0) // FFmpeg headers (C4244 in libavutil/common.h)
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#pragma warning(pop)

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

namespace render_export
{
    namespace
    {
        std::string Utf8(const std::wstring& text)
        {
            if (text.empty())
                return {};
            const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
            return out;
        }

        std::string AvError(int code)
        {
            char text[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(code, text, sizeof(text));
            return text;
        }

        // ------------------------------------------------------------------
        // Runtime: EVER's DLL loop (dllmain.cpp)
        // ------------------------------------------------------------------

        std::mutex g_runtimeMutex;
        bool g_runtimeTried = false;
        bool g_runtimeLoaded = false;
        std::string g_runtimeError;

        std::mutex g_logMutex;
        std::wstring g_logPath;
        bool g_logInstalled = false;

        void LogCallback(void* object, int level, const char* format, va_list args)
        {
            if (level > AV_LOG_WARNING)
                return;
            static int printPrefix = 1;
            char line[1024];
            std::lock_guard<std::mutex> lock(g_logMutex);
            av_log_format_line2(object, level, format, args, line, sizeof(line), &printPrefix);
            if (g_logPath.empty())
                return;
            HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return;
            DWORD written = 0;
            WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
            CloseHandle(file);
        }

        std::vector<std::wstring> RuntimeDlls()
        {
            // The names follow the headers this core was built with
            // (FFmpeg 8.1: avutil-60, avcodec-62...): the vcxproj's
            // /DELAYLOAD list is checked against them at build time.
            return {L"avutil-" + std::to_wstring(LIBAVUTIL_VERSION_MAJOR) + L".dll",
                L"swresample-" + std::to_wstring(LIBSWRESAMPLE_VERSION_MAJOR) + L".dll",
                L"swscale-" + std::to_wstring(LIBSWSCALE_VERSION_MAJOR) + L".dll",
                L"avcodec-" + std::to_wstring(LIBAVCODEC_VERSION_MAJOR) + L".dll",
                L"avformat-" + std::to_wstring(LIBAVFORMAT_VERSION_MAJOR) + L".dll",
                L"avfilter-" + std::to_wstring(LIBAVFILTER_VERSION_MAJOR) + L".dll"};
        }

        void ColourTags(AVCodecContext* ctx, bool hdr10)
        {
            ctx->color_range = AVCOL_RANGE_MPEG;
            ctx->colorspace = hdr10 ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
            ctx->color_primaries = hdr10 ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
            ctx->color_trc = hdr10 ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT709;
        }

        AVDictionary* OptionsDictionary(const EncoderVariant& v, std::uint32_t quality)
        {
            AVDictionary* options = nullptr;
            for (const auto& pair : VariantOptions(v, quality))
                av_dict_set(&options, pair.first.c_str(), pair.second.c_str(), 0);
            return options;
        }

        // One planar frame over the GPU readback buffer (no copy). The
        // buffer goes back to the sink's pool when libavcodec drops the
        // last reference (on the writer thread or an encoder thread).
        struct BufferHolder
        {
            std::vector<std::uint8_t> data;
            void (*recycle)(void* sink, std::vector<std::uint8_t>&& data);
            void* sink;
        };

        void FreeHolder(void* opaque, std::uint8_t*)
        {
            auto* holder = static_cast<BufferHolder*>(opaque);
            holder->recycle(holder->sink, std::move(holder->data));
            delete holder;
        }

        bool FillPlanes(AVFrame* frame, OutputLayout layout, std::uint8_t* base, std::uint32_t width, std::uint32_t height)
        {
            frame->width = static_cast<int>(width);
            frame->height = static_cast<int>(height);
            switch (layout)
            {
            case OutputLayout::Nv12:
                frame->format = AV_PIX_FMT_NV12;
                frame->data[0] = base;
                frame->linesize[0] = static_cast<int>(width);
                frame->data[1] = base + static_cast<std::size_t>(width) * height;
                frame->linesize[1] = static_cast<int>(width);
                return true;
            case OutputLayout::P010:
                frame->format = AV_PIX_FMT_P010LE;
                frame->data[0] = base;
                frame->linesize[0] = static_cast<int>(width * 2);
                frame->data[1] = base + static_cast<std::size_t>(width) * height * 2;
                frame->linesize[1] = static_cast<int>(width * 2);
                return true;
            case OutputLayout::Rgba8:
                frame->format = AV_PIX_FMT_RGBA;
                frame->data[0] = base;
                frame->linesize[0] = static_cast<int>(width * 4);
                return true;
            case OutputLayout::X2Bgr10:
                frame->format = AV_PIX_FMT_X2BGR10LE;
                frame->data[0] = base;
                frame->linesize[0] = static_cast<int>(width * 4);
                return true;
            }
            return false;
        }

        bool YuvLayout(OutputLayout layout)
        {
            return layout == OutputLayout::Nv12 || layout == OutputLayout::P010;
        }
    }

    std::wstring DefaultFfmpegDllFolder()
    {
        return bundled_files::RedemptionEditorFolder() + L"dlls"; // shared with the fonts
    }

    bool LoadFfmpegRuntime(const std::wstring& folder, std::string& error)
    {
        std::lock_guard<std::mutex> lock(g_runtimeMutex);
        if (g_runtimeTried)
        {
            error = g_runtimeError;
            return g_runtimeLoaded;
        }
        g_runtimeTried = true;
        std::wstring base = folder;
        if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
            base += L'\\';
        for (const std::wstring& name : RuntimeDlls())
        {
            const std::wstring path = base + name;
            // The DLL's own imports (x264, x265, libaom...) come from its
            // folder; the process search path is left alone.
            HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module)
            {
                g_runtimeError = "cannot load " + Utf8(path) + " (error " + std::to_string(GetLastError()) + ")";
                error = g_runtimeError;
                return false;
            }
        }
        // Every DLL is in memory: the delay-load stubs bind to them now.
        g_runtimeLoaded = true;
        av_log_set_level(AV_LOG_WARNING);
        av_log_set_callback(LogCallback);
        g_logInstalled = true;
        return true;
    }

    bool FfmpegRuntimeLoaded()
    {
        std::lock_guard<std::mutex> lock(g_runtimeMutex);
        return g_runtimeLoaded;
    }

    std::string FfmpegVersionText()
    {
        if (!FfmpegRuntimeLoaded())
            return "not loaded";
        const unsigned v = avcodec_version();
        char text[160];
        std::snprintf(text, sizeof(text), "%s (avcodec %u.%u.%u)", av_version_info(), v >> 16, (v >> 8) & 0xFF, v & 0xFF);
        return text;
    }

    void ReleaseFfmpegLog()
    {
        if (!FfmpegRuntimeLoaded())
            return;
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logInstalled)
        {
            // The real function in avutil: &av_log_default_callback here
            // would be this core's delay-load thunk, gone after the unload.
            using Callback = void (*)(void*, int, const char*, va_list);
            const std::wstring avutil = L"avutil-" + std::to_wstring(LIBAVUTIL_VERSION_MAJOR) + L".dll";
            const auto standard = reinterpret_cast<Callback>(GetProcAddress(GetModuleHandleW(avutil.c_str()), "av_log_default_callback"));
            av_log_set_callback(standard);
        }
        g_logInstalled = false;
        g_logPath.clear();
    }

    void SetFfmpegLogFile(const std::wstring& path)
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_logPath = path;
    }

    // ------------------------------------------------------------------
    // Probe
    // ------------------------------------------------------------------

    ProbeResult ProbeEncoders()
    {
        ProbeResult result;
        const auto& variants = AllVariants();
        result.opened.assign(variants.size(), false);
        result.notes.assign(variants.size(), std::string());
        result.runtimeLoaded = FfmpegRuntimeLoaded();
        if (!result.runtimeLoaded)
        {
            result.done = true;
            return result;
        }
        std::map<std::string, std::pair<bool, std::string>> cache;
        for (std::size_t i = 0; i < variants.size(); ++i)
        {
            const EncoderVariant& v = variants[i];
            const std::string key = std::string(v.codec) + "|" + v.pixFmt + "|" + v.options;
            auto known = cache.find(key);
            if (known != cache.end())
            {
                result.opened[i] = known->second.first;
                result.notes[i] = known->second.second;
                continue;
            }
            bool opened = false;
            std::string note;
            const AVCodec* codec = avcodec_find_encoder_by_name(v.codec);
            if (!codec)
                note = "not in this FFmpeg build";
            else if (AVCodecContext* ctx = avcodec_alloc_context3(codec))
            {
                ctx->width = 1280;
                ctx->height = 720;
                ctx->time_base = {1, 60};
                ctx->framerate = {60, 1};
                ctx->sample_aspect_ratio = {1, 1};
                ctx->pix_fmt = av_get_pix_fmt(v.pixFmt);
                if (IsYuvPixFmt(v.pixFmt))
                    ColourTags(ctx, false);
                AVDictionary* options = OptionsDictionary(v, 0);
                const int ret = avcodec_open2(ctx, codec, &options);
                av_dict_free(&options);
                opened = ret >= 0;
                if (!opened)
                    note = AvError(ret);
                avcodec_free_context(&ctx);
            }
            cache[key] = {opened, note};
            result.opened[i] = opened;
            result.notes[i] = note;
        }
        result.done = true;
        return result;
    }

    // ------------------------------------------------------------------
    // LibavSink
    // ------------------------------------------------------------------

    LibavSink::~LibavSink()
    {
        Close();
        Free();
    }

    bool LibavSink::Open(const EncodeJob& job)
    {
        m_job = job;
        if (!FfmpegRuntimeLoaded())
        {
            Fail(ExportError::EncoderStart);
            return false;
        }
        SetFfmpegLogFile(job.output + L".ffmpeg.log");
        StartWriter();
        return true;
    }

    std::string LibavSink::EncoderName() const
    {
        std::lock_guard<std::mutex> lock(m_openMutex);
        return m_opened ? m_encoderName : std::string();
    }

    std::string LibavSink::OpenError() const
    {
        std::lock_guard<std::mutex> lock(m_openMutex);
        return m_openError;
    }

    bool LibavSink::BeginBackend()
    {
        // The probe's choice first, then the rest of the preset's variants
        // in order (an export started without the dialog, a GPU session
        // refused right now by the driver).
        std::vector<int> order;
        if (m_job.variant >= 0)
            order.push_back(m_job.variant);
        for (std::size_t index : VariantsOf(m_job.preset))
        {
            const EncoderVariant& v = AllVariants()[index];
            if (!m_job.automaticBackend && v.backend != static_cast<EncoderBackend>(m_job.forcedBackend))
            {
                bool presetHasForced = false;
                for (std::size_t other : VariantsOf(m_job.preset))
                    presetHasForced = presetHasForced || AllVariants()[other].backend == static_cast<EncoderBackend>(m_job.forcedBackend);
                if (presetHasForced)
                    continue;
            }
            if (static_cast<int>(index) != m_job.variant)
                order.push_back(static_cast<int>(index));
        }
        std::string errors;
        for (int index : order)
        {
            std::string error;
            if (OpenVariant(index, error))
            {
                std::lock_guard<std::mutex> lock(m_openMutex);
                m_opened = true;
                return true;
            }
            errors += std::string(AllVariants()[static_cast<std::size_t>(index)].codec) + ": " + error + "; ";
            Free();
        }
        {
            std::lock_guard<std::mutex> lock(m_openMutex);
            m_openError = errors.empty() ? "no encoder for this preset" : errors;
        }
        av_log(nullptr, AV_LOG_ERROR, "Redemption Editor: no encoder opened: %s\n", errors.c_str());
        return false;
    }

    bool LibavSink::OpenVariant(int index, std::string& error)
    {
        const EncoderVariant& v = AllVariants()[static_cast<std::size_t>(index)];
        const bool hdr10 = m_job.hdr10;
        const std::string pixFmt = VariantPixFmt(v, hdr10);
        const AVCodec* codec = avcodec_find_encoder_by_name(v.codec);
        if (!codec)
        {
            error = "not in this FFmpeg build";
            return false;
        }
        const std::string output = m_job.preset == VideoPreset::PngSequence ? Utf8(m_job.output + L"\\%06d.png") : Utf8(m_job.output);
        int ret = avformat_alloc_output_context2(&m_format, nullptr, MuxerFor(m_job.output, m_job.preset), output.c_str());
        if (ret < 0 || !m_format)
        {
            error = "muxer: " + AvError(ret);
            return false;
        }
        m_codec = avcodec_alloc_context3(codec);
        if (!m_codec)
        {
            error = "no codec context";
            return false;
        }
        std::uint32_t width = m_job.width;
        std::uint32_t height = m_job.height;
        if (m_job.preset == VideoPreset::PreviewH264)
        {
            width = width / 4 * 2;
            height = height / 4 * 2;
        }
        m_codec->width = static_cast<int>(width);
        m_codec->height = static_cast<int>(height);
        m_codec->time_base = {static_cast<int>(m_job.rate.den), static_cast<int>(m_job.rate.num)};
        m_codec->framerate = {static_cast<int>(m_job.rate.num), static_cast<int>(m_job.rate.den)};
        m_codec->sample_aspect_ratio = {1, 1};
        m_codec->pix_fmt = av_get_pix_fmt(pixFmt.c_str());
        m_codec->thread_count = 0; // the CPU encoders: one thread per core (auto)
        if (IsYuvPixFmt(pixFmt))
            ColourTags(m_codec, hdr10);
        if (m_format->oformat->flags & AVFMT_GLOBALHEADER)
            m_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary* options = OptionsDictionary(v, m_job.quality);
        ret = avcodec_open2(m_codec, codec, &options);
        av_dict_free(&options);
        if (ret < 0)
        {
            error = "open: " + AvError(ret);
            return false;
        }
        const std::string chain = VideoFilterChain(m_job.preset, m_job.layout, pixFmt, hdr10, Utf8(m_job.lut));
        if (!chain.empty() && !OpenFilters(chain, error))
            return false;
        m_stream = avformat_new_stream(m_format, nullptr);
        if (!m_stream)
        {
            error = "no stream";
            return false;
        }
        avcodec_parameters_from_context(m_stream->codecpar, m_codec);
        m_stream->time_base = m_codec->time_base;
        m_stream->avg_frame_rate = m_codec->framerate;
        if (!(m_format->oformat->flags & AVFMT_NOFILE))
        {
            ret = avio_open(&m_format->pb, output.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0)
            {
                error = "file: " + AvError(ret);
                return false;
            }
        }
        // Fragmented MP4/MOV, Matroska clusters of
        // a second (encoder_select.h CrashSafeMuxOptions): a file cut by a
        // crash still opens. MOV keeps +write_colr in the same flags.
        // Each written packet (a whole fragment or cluster, as the muxer emits
        // them) goes to the file at once, not into AVIOContext's 32 KB buffer
        // that a crash would lose.
        m_format->flush_packets = 1;
        AVDictionary* mux = nullptr;
        for (const auto& option : CrashSafeMuxOptions(m_format->oformat->name, m_job.preset))
            av_dict_set(&mux, option.first.c_str(), option.second.c_str(), 0);
        ret = avformat_write_header(m_format, &mux);
        for (const AVDictionaryEntry* left = av_dict_get(mux, "", nullptr, AV_DICT_IGNORE_SUFFIX); left;
             left = av_dict_get(mux, "", left, AV_DICT_IGNORE_SUFFIX))
            av_log(nullptr, AV_LOG_WARNING, "Redemption Editor: muxer option %s=%s not taken by %s\n", left->key, left->value, m_format->oformat->name);
        av_dict_free(&mux);
        if (ret < 0)
        {
            error = "header: " + AvError(ret);
            return false;
        }
        m_headerWritten = true;
        m_packet = av_packet_alloc();
        m_filtered = av_frame_alloc();
        m_variant = index;
        m_encoderName = v.codec;
        return true;
    }

    bool LibavSink::OpenFilters(const std::string& chain, std::string& error)
    {
        m_graph = avfilter_graph_alloc();
        if (!m_graph)
        {
            error = "no filter graph";
            return false;
        }
        const bool yuv = YuvLayout(m_job.layout);
        char args[512];
        std::snprintf(args, sizeof(args), "video_size=%ux%u:pix_fmt=%d:time_base=%u/%u:pixel_aspect=1/1:frame_rate=%u/%u:colorspace=%d:range=%d",
            m_job.width, m_job.height, static_cast<int>(av_get_pix_fmt(LayoutPixFmt(m_job.layout))), m_job.rate.den, m_job.rate.num, m_job.rate.num,
            m_job.rate.den, yuv ? static_cast<int>(m_job.hdr10 ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709) : static_cast<int>(AVCOL_SPC_RGB),
            yuv ? static_cast<int>(AVCOL_RANGE_MPEG) : static_cast<int>(AVCOL_RANGE_JPEG));
        int ret = avfilter_graph_create_filter(&m_source, avfilter_get_by_name("buffer"), "in", args, nullptr, m_graph);
        if (ret < 0)
        {
            error = "buffer: " + AvError(ret);
            return false;
        }
        ret = avfilter_graph_create_filter(&m_sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, m_graph);
        if (ret < 0)
        {
            error = "buffersink: " + AvError(ret);
            return false;
        }
        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs = avfilter_inout_alloc();
        outputs->name = av_strdup("in");
        outputs->filter_ctx = m_source;
        outputs->pad_idx = 0;
        outputs->next = nullptr;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = m_sink;
        inputs->pad_idx = 0;
        inputs->next = nullptr;
        ret = avfilter_graph_parse_ptr(m_graph, chain.c_str(), &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (ret < 0)
        {
            error = "filters (" + chain + "): " + AvError(ret);
            return false;
        }
        ret = avfilter_graph_config(m_graph, nullptr);
        if (ret < 0)
        {
            error = "filters config (" + chain + "): " + AvError(ret);
            return false;
        }
        return true;
    }

    bool LibavSink::Consume(VideoFrame& frame)
    {
        AVFrame* in = av_frame_alloc();
        if (!in)
            return false;
        auto* holder = new BufferHolder{std::move(frame.data), nullptr, this};
        holder->recycle = [](void* sink, std::vector<std::uint8_t>&& data) { static_cast<LibavSink*>(sink)->Recycle(std::move(data)); };
        FillPlanes(in, m_job.layout, holder->data.data(), m_job.width, m_job.height);
        in->buf[0] = av_buffer_create(holder->data.data(), holder->data.size(), FreeHolder, holder, 0);
        if (!in->buf[0])
        {
            frame.data = std::move(holder->data);
            delete holder;
            av_frame_free(&in);
            return false;
        }
        const bool yuv = YuvLayout(m_job.layout);
        in->color_range = yuv ? AVCOL_RANGE_MPEG : AVCOL_RANGE_JPEG;
        in->colorspace = yuv ? (m_job.hdr10 ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709) : AVCOL_SPC_RGB;
        in->color_primaries = m_job.hdr10 ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
        in->color_trc = m_job.hdr10 ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT709;
        in->sample_aspect_ratio = {1, 1};
        in->pts = m_pts++;
        bool ok = true;
        if (!m_graph)
        {
            ok = Encode(in);
        }
        else
        {
            int ret = av_buffersrc_add_frame_flags(m_source, in, 0);
            if (ret < 0)
                ok = false;
            while (ok)
            {
                ret = av_buffersink_get_frame(m_sink, m_filtered);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0)
                {
                    ok = false;
                    break;
                }
                ok = Encode(m_filtered);
                av_frame_unref(m_filtered);
            }
        }
        av_frame_free(&in);
        return ok;
    }

    bool LibavSink::Encode(AVFrame* frame)
    {
        if (frame && !m_thumbnailDone && !m_job.thumbnail.empty() && frame->pts >= static_cast<std::int64_t>(m_job.thumbnailFrame))
        {
            m_thumbnailDone = true;
            Thumbnail(frame);
        }
        if (frame && m_codec->pix_fmt != frame->format)
        {
            // Never expected: the filter chain ends with format=<encoder's>.
            return false;
        }
        const int ret = avcodec_send_frame(m_codec, frame);
        if (ret < 0 && ret != AVERROR_EOF)
            return false;
        return Drain();
    }

    bool LibavSink::Drain()
    {
        for (;;)
        {
            const int ret = avcodec_receive_packet(m_codec, m_packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                return true;
            if (ret < 0)
                return false;
            av_packet_rescale_ts(m_packet, m_codec->time_base, m_stream->time_base);
            m_packet->stream_index = m_stream->index;
            const int written = av_interleaved_write_frame(m_format, m_packet);
            av_packet_unref(m_packet);
            if (written < 0)
                return false;
        }
    }

    // JPEG 1280 wide from the frame going into the encoder (after the LUT):
    // full range BT.601 like any JPEG (the ffmpeg.exe path's
    // scale=1280:-2:out_range=pc,format=yuvj420p).
    void LibavSink::Thumbnail(const AVFrame* frame)
    {
        const int w = 1280;
        const int h = std::max(2, static_cast<int>(std::lround(static_cast<double>(frame->height) * w / frame->width / 2.0)) * 2);
        const auto source = static_cast<AVPixelFormat>(frame->format);
        SwsContext* sws = sws_getContext(frame->width, frame->height, source, w, h, AV_PIX_FMT_YUVJ420P, SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!sws)
            return;
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(source);
        const bool rgb = desc && (desc->flags & AV_PIX_FMT_FLAG_RGB);
        const int matrix = m_job.hdr10 ? SWS_CS_BT2020 : SWS_CS_ITU709;
        sws_setColorspaceDetails(sws, sws_getCoefficients(matrix), rgb ? 1 : 0, sws_getCoefficients(SWS_CS_ITU601), 1, 0, 1 << 16, 1 << 16);
        AVFrame* thumb = av_frame_alloc();
        thumb->format = AV_PIX_FMT_YUVJ420P;
        thumb->width = w;
        thumb->height = h;
        const AVCodec* mjpeg = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        AVCodecContext* ctx = mjpeg ? avcodec_alloc_context3(mjpeg) : nullptr;
        AVPacket* packet = av_packet_alloc();
        if (ctx && av_frame_get_buffer(thumb, 0) >= 0)
        {
            sws_scale(sws, frame->data, frame->linesize, 0, frame->height, thumb->data, thumb->linesize);
            ctx->width = w;
            ctx->height = h;
            ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
            ctx->color_range = AVCOL_RANGE_JPEG;
            ctx->time_base = {1, 25};
            ctx->flags |= AV_CODEC_FLAG_QSCALE;
            ctx->global_quality = FF_QP2LAMBDA * 3;
            thumb->quality = ctx->global_quality;
            thumb->pts = 0;
            if (avcodec_open2(ctx, mjpeg, nullptr) >= 0 && avcodec_send_frame(ctx, thumb) >= 0 && avcodec_send_frame(ctx, nullptr) >= 0 &&
                avcodec_receive_packet(ctx, packet) >= 0)
            {
                HANDLE file = CreateFileW(m_job.thumbnail.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE)
                {
                    DWORD written = 0;
                    WriteFile(file, packet->data, static_cast<DWORD>(packet->size), &written, nullptr);
                    CloseHandle(file);
                }
            }
        }
        av_packet_free(&packet);
        avcodec_free_context(&ctx);
        av_frame_free(&thumb);
        sws_freeContext(sws);
    }

    bool LibavSink::CloseBackend()
    {
        bool ok = m_codec && m_headerWritten;
        if (ok && Error() == ExportError::None)
        {
            if (m_graph)
            {
                // Flush the chain, then the encoder.
                if (av_buffersrc_add_frame_flags(m_source, nullptr, 0) >= 0)
                {
                    for (;;)
                    {
                        const int ret = av_buffersink_get_frame(m_sink, m_filtered);
                        if (ret < 0)
                            break;
                        ok = Encode(m_filtered) && ok;
                        av_frame_unref(m_filtered);
                    }
                }
            }
            ok = Encode(nullptr) && ok;
        }
        if (m_headerWritten)
        {
            ok = av_write_trailer(m_format) >= 0 && ok;
            m_headerWritten = false;
        }
        Free();
        SetFfmpegLogFile(L"");
        return ok;
    }

    void LibavSink::Free()
    {
        if (!FfmpegRuntimeLoaded())
            return;
        avfilter_graph_free(&m_graph);
        m_source = nullptr;
        m_sink = nullptr;
        avcodec_free_context(&m_codec); // drops the last frame references (back to the pool)
        if (m_format)
        {
            if (m_format->pb && !(m_format->oformat->flags & AVFMT_NOFILE))
                avio_closep(&m_format->pb);
            avformat_free_context(m_format);
            m_format = nullptr;
        }
        m_stream = nullptr;
        av_frame_free(&m_filtered);
        av_packet_free(&m_packet);
    }

    // ------------------------------------------------------------------
    // Finalize (video copied, audio pass, chapters)
    // ------------------------------------------------------------------

    namespace
    {
        struct FinalizeState
        {
            AVFormatContext* video = nullptr;
            AVFormatContext* wav = nullptr;
            AVFormatContext* out = nullptr;
            AVCodecContext* decoder = nullptr;
            AVCodecContext* encoder = nullptr;
            SwrContext* swr = nullptr;
            AVAudioFifo* fifo = nullptr;
            AVFrame* decoded = nullptr;
            AVFrame* chunk = nullptr;
            AVPacket* packet = nullptr;      // the video copy
            AVPacket* wavPacket = nullptr;   // read from the WAV
            AVPacket* audioPacket = nullptr; // out of the audio encoder
            AVStream* videoOut = nullptr;
            AVStream* audioOut = nullptr;
            int videoIn = -1;
            int audioIn = -1;
            std::int64_t skip = 0;       // samples to drop before frame 0
            std::int64_t limit = 0;      // samples of the video's length
            std::int64_t written = 0;    // samples sent to the encoder
            bool wavEnded = false;

            ~FinalizeState()
            {
                av_frame_free(&decoded);
                av_frame_free(&chunk);
                av_packet_free(&packet);
                av_packet_free(&wavPacket);
                av_packet_free(&audioPacket);
                if (fifo)
                    av_audio_fifo_free(fifo);
                swr_free(&swr);
                avcodec_free_context(&decoder);
                avcodec_free_context(&encoder);
                if (video)
                    avformat_close_input(&video);
                if (wav)
                    avformat_close_input(&wav);
                if (out)
                {
                    if (out->pb)
                        avio_closep(&out->pb);
                    avformat_free_context(out);
                }
            }

            bool WriteAudioPackets()
            {
                for (;;)
                {
                    const int ret = avcodec_receive_packet(encoder, audioPacket);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        return true;
                    if (ret < 0)
                        return false;
                    av_packet_rescale_ts(audioPacket, encoder->time_base, audioOut->time_base);
                    audioPacket->stream_index = audioOut->index;
                    if (av_interleaved_write_frame(out, audioPacket) < 0)
                        return false;
                }
            }

            // Decodes the WAV until the FIFO holds `want` samples (or it ends).
            bool Fill(int want)
            {
                while (!wavEnded && av_audio_fifo_size(fifo) < want)
                {
                    int ret = av_read_frame(wav, wavPacket);
                    if (ret < 0)
                    {
                        avcodec_send_packet(decoder, nullptr);
                        wavEnded = true;
                    }
                    else
                    {
                        if (wavPacket->stream_index == audioIn)
                            avcodec_send_packet(decoder, wavPacket);
                        av_packet_unref(wavPacket);
                    }
                    while ((ret = avcodec_receive_frame(decoder, decoded)) >= 0)
                    {
                        const int capacity = swr_get_out_samples(swr, decoded->nb_samples);
                        std::uint8_t** planes = nullptr;
                        if (av_samples_alloc_array_and_samples(&planes, nullptr, encoder->ch_layout.nb_channels, capacity, encoder->sample_fmt, 0) < 0)
                            return false;
                        const int converted = swr_convert(swr, planes, capacity, const_cast<const std::uint8_t**>(decoded->extended_data), decoded->nb_samples);
                        av_frame_unref(decoded);
                        int start = 0;
                        if (converted > 0 && skip > 0)
                        {
                            start = static_cast<int>(std::min<std::int64_t>(skip, converted));
                            skip -= start;
                        }
                        if (converted > start)
                        {
                            // Offset each plane by `start` samples.
                            const bool planar = av_sample_fmt_is_planar(encoder->sample_fmt) != 0;
                            const int bytes = av_get_bytes_per_sample(encoder->sample_fmt) * (planar ? 1 : encoder->ch_layout.nb_channels);
                            void* shifted[AV_NUM_DATA_POINTERS]{};
                            const int planesCount = planar ? encoder->ch_layout.nb_channels : 1;
                            for (int p = 0; p < planesCount && p < AV_NUM_DATA_POINTERS; ++p)
                                shifted[p] = planes[p] + static_cast<std::size_t>(start) * bytes;
                            av_audio_fifo_write(fifo, shifted, converted - start);
                        }
                        av_freep(&planes[0]);
                        av_freep(&planes);
                    }
                }
                return true;
            }

            // Encodes audio up to `untilSample` (never past the limit).
            bool EncodeUntil(std::int64_t untilSample, bool final)
            {
                untilSample = std::min(untilSample, limit);
                const int frameSize = encoder->frame_size > 0 ? encoder->frame_size : 1024;
                while (written < untilSample)
                {
                    const int want = static_cast<int>(std::min<std::int64_t>(frameSize, limit - written));
                    if (!Fill(want))
                        return false;
                    const int have = std::min(av_audio_fifo_size(fifo), want);
                    if (have <= 0)
                        break; // the WAV ended early
                    if (have < frameSize && !final && !wavEnded && written + have < limit)
                        break;
                    av_frame_unref(chunk);
                    chunk->nb_samples = have;
                    chunk->format = encoder->sample_fmt;
                    av_channel_layout_copy(&chunk->ch_layout, &encoder->ch_layout);
                    chunk->sample_rate = encoder->sample_rate;
                    if (av_frame_get_buffer(chunk, 0) < 0)
                        return false;
                    av_audio_fifo_read(fifo, reinterpret_cast<void**>(chunk->data), have);
                    chunk->pts = written;
                    written += have;
                    if (avcodec_send_frame(encoder, chunk) < 0 || !WriteAudioPackets())
                        return false;
                }
                return true;
            }
        };
    }

    bool LibavFinalize(const EncodeJob& job, const std::wstring& videoPath, double audioStartS, const std::vector<Chapter>& chapters,
        std::uint64_t durationMs, std::uint64_t frames, std::string& error)
    {
        return LibavFinalizeParts(job, {{videoPath, frames}}, audioStartS, chapters, durationMs, frames, error);
    }

    bool LibavFinalizeParts(const EncodeJob& job, const std::vector<VideoPart>& parts, double audioStartS, const std::vector<Chapter>& chapters,
        std::uint64_t durationMs, std::uint64_t frames, std::string& error)
    {
        if (!FfmpegRuntimeLoaded())
        {
            error = "ffmpeg_runtime_missing";
            return false;
        }
        if (parts.empty())
        {
            error = "no video";
            return false;
        }
        FinalizeState s;
        const std::string videoUtf8 = Utf8(parts.front().path);
        const std::string finalUtf8 = Utf8(job.finalOutput);
        int ret = avformat_open_input(&s.video, videoUtf8.c_str(), nullptr, nullptr);
        if (ret < 0 || avformat_find_stream_info(s.video, nullptr) < 0)
        {
            error = "open video: " + AvError(ret);
            return false;
        }
        s.videoIn = av_find_best_stream(s.video, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (s.videoIn < 0)
        {
            error = "no video stream";
            return false;
        }
        ret = avformat_alloc_output_context2(&s.out, nullptr, MuxerFor(job.finalOutput, job.preset), finalUtf8.c_str());
        if (ret < 0 || !s.out)
        {
            error = "muxer: " + AvError(ret);
            return false;
        }
        AVStream* inVideo = s.video->streams[s.videoIn];
        s.videoOut = avformat_new_stream(s.out, nullptr);
        avcodec_parameters_copy(s.videoOut->codecpar, inVideo->codecpar);
        s.videoOut->codecpar->codec_tag = 0;
        s.videoOut->time_base = inVideo->time_base;
        s.videoOut->avg_frame_rate = inVideo->avg_frame_rate;

        // Audio: the WAV of the audio pass, from frame 0, cut at the video's length.
        if (!job.audioWav.empty())
        {
            const std::string wavUtf8 = Utf8(job.audioWav);
            ret = avformat_open_input(&s.wav, wavUtf8.c_str(), nullptr, nullptr);
            if (ret < 0 || avformat_find_stream_info(s.wav, nullptr) < 0)
            {
                error = "open wav: " + AvError(ret);
                return false;
            }
            s.audioIn = av_find_best_stream(s.wav, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (s.audioIn < 0)
            {
                error = "no audio in the wav";
                return false;
            }
            const AVCodecParameters* par = s.wav->streams[s.audioIn]->codecpar;
            const AVCodec* decoder = avcodec_find_decoder(par->codec_id);
            const AVCodec* encoder = avcodec_find_encoder_by_name(FinalAudioCodec(job.finalOutput));
            if (!decoder || !encoder)
            {
                error = "audio codec missing";
                return false;
            }
            s.decoder = avcodec_alloc_context3(decoder);
            avcodec_parameters_to_context(s.decoder, par);
            if (avcodec_open2(s.decoder, decoder, nullptr) < 0)
            {
                error = "wav decoder";
                return false;
            }
            s.encoder = avcodec_alloc_context3(encoder);
            s.encoder->sample_rate = s.decoder->sample_rate;
            if (s.decoder->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
                av_channel_layout_default(&s.encoder->ch_layout, s.decoder->ch_layout.nb_channels);
            else
                av_channel_layout_copy(&s.encoder->ch_layout, &s.decoder->ch_layout);
            // The sample format: FLAC and PCM in 24 bit (s32), AAC its own.
            const void* formats = nullptr;
            int count = 0;
            avcodec_get_supported_config(s.encoder, encoder, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &formats, &count);
            const auto* list = static_cast<const AVSampleFormat*>(formats);
            s.encoder->sample_fmt = list && count > 0 ? list[0] : AV_SAMPLE_FMT_FLTP;
            for (int i = 0; list && i < count; ++i)
                if (list[i] == AV_SAMPLE_FMT_S32)
                    s.encoder->sample_fmt = AV_SAMPLE_FMT_S32;
            if (s.encoder->sample_fmt == AV_SAMPLE_FMT_S32)
                s.encoder->bits_per_raw_sample = 24;
            if (encoder->id == AV_CODEC_ID_AAC)
                s.encoder->bit_rate = 320000;
            s.encoder->time_base = {1, s.encoder->sample_rate};
            if (s.out->oformat->flags & AVFMT_GLOBALHEADER)
                s.encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            ret = avcodec_open2(s.encoder, encoder, nullptr);
            if (ret < 0)
            {
                error = "audio encoder: " + AvError(ret);
                return false;
            }
            s.audioOut = avformat_new_stream(s.out, nullptr);
            avcodec_parameters_from_context(s.audioOut->codecpar, s.encoder);
            s.audioOut->time_base = s.encoder->time_base;
            if (swr_alloc_set_opts2(&s.swr, &s.encoder->ch_layout, s.encoder->sample_fmt, s.encoder->sample_rate, &s.decoder->ch_layout,
                    s.decoder->sample_fmt, s.decoder->sample_rate, 0, nullptr) < 0 ||
                swr_init(s.swr) < 0)
            {
                error = "resampler";
                return false;
            }
            s.fifo = av_audio_fifo_alloc(s.encoder->sample_fmt, s.encoder->ch_layout.nb_channels, 4096);
            s.decoded = av_frame_alloc();
            s.chunk = av_frame_alloc();
            s.skip = static_cast<std::int64_t>(std::llround(std::max(0.0, audioStartS) * s.encoder->sample_rate));
            s.limit = static_cast<std::int64_t>(frames * job.rate.den * static_cast<std::uint64_t>(s.encoder->sample_rate) / job.rate.num);
        }
        s.packet = av_packet_alloc();
        s.wavPacket = av_packet_alloc();
        s.audioPacket = av_packet_alloc();

        // Chapters (the markers): a chapter runs to the next one, the last to the end.
        for (std::size_t i = 0; i < chapters.size(); ++i)
        {
            const std::uint64_t start = chapters[i].startMs;
            const std::uint64_t end = i + 1 < chapters.size() ? chapters[i + 1].startMs : durationMs;
            if (end <= start)
                continue;
            auto* chapter = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
            if (!chapter)
                continue;
            chapter->id = static_cast<std::int64_t>(i);
            chapter->time_base = {1, 1000};
            chapter->start = static_cast<std::int64_t>(start);
            chapter->end = static_cast<std::int64_t>(end);
            av_dict_set(&chapter->metadata, "title", chapters[i].title.c_str(), 0);
            if (av_dynarray_add_nofree(&s.out->chapters, reinterpret_cast<int*>(&s.out->nb_chapters), chapter) < 0)
            {
                av_dict_free(&chapter->metadata);
                av_free(chapter);
            }
        }

        ret = avio_open(&s.out->pb, finalUtf8.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            error = "final file: " + AvError(ret);
            return false;
        }
        AVDictionary* mux = nullptr;
        const std::string muxer = s.out->oformat->name;
        if (muxer == "mp4")
            av_dict_set(&mux, "movflags", "+faststart", 0);
        else if (muxer == "mov")
            av_dict_set(&mux, "movflags", "+write_colr", 0);
        ret = avformat_write_header(s.out, &mux);
        av_dict_free(&mux);
        if (ret < 0)
        {
            error = "header: " + AvError(ret);
            return false;
        }
        // Video packets in order, the audio kept half a second ahead. A whole
        // project's pieces one after the other: piece k's timestamps moved to
        // its first frame (the frames of the pieces before it at the export's
        // rate), whatever each file's own clock says, so the seams neither
        // gap nor overlap.
        const AVCodecID firstCodec = inVideo->codecpar->codec_id;
        const int firstWidth = inVideo->codecpar->width, firstHeight = inVideo->codecpar->height, firstFormat = inVideo->codecpar->format;
        const std::int64_t firstStart = inVideo->start_time != AV_NOPTS_VALUE ? av_rescale_q(inVideo->start_time, inVideo->time_base, s.videoOut->time_base) : 0;
        std::uint64_t partFirstFrame = 0;
        for (std::size_t part = 0; part < parts.size(); ++part)
        {
            std::int64_t shift = 0;
            if (part > 0)
            {
                avformat_close_input(&s.video);
                const std::string partUtf8 = Utf8(parts[part].path);
                ret = avformat_open_input(&s.video, partUtf8.c_str(), nullptr, nullptr);
                if (ret < 0 || avformat_find_stream_info(s.video, nullptr) < 0)
                {
                    error = "open piece " + std::to_string(part + 1) + ": " + AvError(ret);
                    return false;
                }
                s.videoIn = av_find_best_stream(s.video, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                if (s.videoIn < 0)
                {
                    error = "no video stream in piece " + std::to_string(part + 1);
                    return false;
                }
                inVideo = s.video->streams[s.videoIn];
                const AVCodecParameters* par = inVideo->codecpar;
                if (par->codec_id != firstCodec || par->width != firstWidth || par->height != firstHeight || par->format != firstFormat)
                {
                    error = "piece " + std::to_string(part + 1) + " is not encoded as the first one";
                    return false;
                }
                const std::int64_t start = inVideo->start_time != AV_NOPTS_VALUE ? av_rescale_q(inVideo->start_time, inVideo->time_base, s.videoOut->time_base) : 0;
                const AVRational frame{static_cast<int>(job.rate.den), static_cast<int>(job.rate.num)};
                shift = av_rescale_q(static_cast<std::int64_t>(partFirstFrame), frame, s.videoOut->time_base) + firstStart - start;
            }
            while ((ret = av_read_frame(s.video, s.packet)) >= 0)
            {
                if (s.packet->stream_index != s.videoIn)
                {
                    av_packet_unref(s.packet);
                    continue;
                }
                // A video pass cut by a crash ends in a
                // fragment written halfway; the demuxer flags its packet corrupt.
                // Everything before it is whole: the copy stops there.
                if (s.packet->flags & AV_PKT_FLAG_CORRUPT)
                {
                    av_packet_unref(s.packet);
                    break;
                }
                av_packet_rescale_ts(s.packet, inVideo->time_base, s.videoOut->time_base);
                if (s.packet->pts != AV_NOPTS_VALUE)
                    s.packet->pts += shift;
                if (s.packet->dts != AV_NOPTS_VALUE)
                    s.packet->dts += shift;
                if (s.encoder && s.packet->pts != AV_NOPTS_VALUE)
                {
                    const double t = static_cast<double>(s.packet->pts) * av_q2d(s.videoOut->time_base) + 0.5;
                    if (!s.EncodeUntil(static_cast<std::int64_t>(t * s.encoder->sample_rate), false))
                    {
                        error = "audio encode";
                        return false;
                    }
                }
                s.packet->stream_index = s.videoOut->index;
                s.packet->pos = -1;
                if (av_interleaved_write_frame(s.out, s.packet) < 0)
                {
                    error = "write video";
                    return false;
                }
            }
            partFirstFrame += parts[part].frames;
        }
        if (s.encoder)
        {
            if (!s.EncodeUntil(s.limit, true) || avcodec_send_frame(s.encoder, nullptr) < 0 || !s.WriteAudioPackets())
            {
                error = "audio flush";
                return false;
            }
        }
        if (av_write_trailer(s.out) < 0)
        {
            error = "trailer";
            return false;
        }
        return true;
    }
}
