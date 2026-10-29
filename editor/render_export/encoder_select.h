#pragma once

// Render export: which libavcodec encoder runs a preset on this PC, with which
// options. A preset is a list of variants, best first: NVENC, AMF, QSV, then the
// CPU. The export dialog probes them once (avcodec_open2 on a small frame) and
// shows only the presets with a variant that opened. When the GPU resolve layout
// differs from the encoder's pixel format, or with a LUT or the preview scale, the
// frame goes through a libavfilter chain (VideoFilterChain). Pure: no FFmpeg types.

#include "convert_ref.h"
#include "export_types.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace render_export
{
    using replay::exportchannel::VideoPreset;

    enum class EncoderBackend : std::uint32_t
    {
        Nvenc = 0,
        Amf = 1,
        Qsv = 2,
        Cpu = 3
    };

    inline const char* BackendName(EncoderBackend backend) noexcept
    {
        switch (backend)
        {
        case EncoderBackend::Nvenc: return "NVENC";
        case EncoderBackend::Amf: return "AMF";
        case EncoderBackend::Qsv: return "QSV";
        case EncoderBackend::Cpu: return "CPU";
        }
        return "?";
    }

    // [Export] Encoder= in RedemptionEditor.ini: auto (the first variant that
    // opens), or one backend only (nvenc, amf, qsv, cpu).
    inline bool ParseBackendChoice(const std::wstring& text, bool& automatic, EncoderBackend& backend)
    {
        std::wstring lower;
        for (wchar_t c : text)
            lower += (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        automatic = true;
        backend = EncoderBackend::Nvenc;
        if (lower.empty() || lower == L"auto")
            return true;
        automatic = false;
        if (lower == L"nvenc" || lower == L"nvidia")
            backend = EncoderBackend::Nvenc;
        else if (lower == L"amf" || lower == L"amd")
            backend = EncoderBackend::Amf;
        else if (lower == L"qsv" || lower == L"intel")
            backend = EncoderBackend::Qsv;
        else if (lower == L"cpu" || lower == L"software")
            backend = EncoderBackend::Cpu;
        else
        {
            automatic = true;
            return false;
        }
        return true;
    }

    // One way to run a preset. `options` is "key=value|key=value": each pair goes
    // to the encoder's private options, then to the codec context. "{q}" is
    // replaced by the quality (CQ, QP or CRF): the job's, or `defaultQuality`.
    struct EncoderVariant
    {
        VideoPreset preset;
        EncoderBackend backend;
        const char* codec;      // avcodec_find_encoder_by_name
        const char* pixFmt;     // what the encoder is fed (av_get_pix_fmt)
        OutputLayout input;     // what the GPU resolve writes for it (SDR)
        const char* options;
        std::uint32_t defaultQuality;
    };

    // Best first. The NVENC settings were measured with ffmpeg.exe.
    inline const std::vector<EncoderVariant>& AllVariants()
    {
        static const std::vector<EncoderVariant> variants = {
            // H.264
            {VideoPreset::H264Nvenc, EncoderBackend::Nvenc, "h264_nvenc", "nv12", OutputLayout::Nv12, "preset=p6|tune=hq|rc=vbr|b=0|cq={q}|profile=high", 19},
            {VideoPreset::H264Nvenc, EncoderBackend::Amf, "h264_amf", "nv12", OutputLayout::Nv12, "usage=transcoding|quality=quality|rc=cqp|qp_i={q}|qp_p={q}|qp_b={q}|profile=high", 20},
            {VideoPreset::H264Nvenc, EncoderBackend::Qsv, "h264_qsv", "nv12", OutputLayout::Nv12, "preset=veryslow|global_quality={q}|profile=high", 21},
            {VideoPreset::H264Nvenc, EncoderBackend::Cpu, "libx264", "nv12", OutputLayout::Nv12, "preset=slow|crf={q}|profile=high", 18},
            // H.265 10 bit
            {VideoPreset::HevcNvenc, EncoderBackend::Nvenc, "hevc_nvenc", "p010le", OutputLayout::P010, "preset=p6|tune=hq|rc=vbr|b=0|profile=main10|cq={q}", 20},
            {VideoPreset::HevcNvenc, EncoderBackend::Amf, "hevc_amf", "p010le", OutputLayout::P010, "usage=transcoding|quality=quality|rc=cqp|qp_i={q}|qp_p={q}|profile=main10", 22},
            {VideoPreset::HevcNvenc, EncoderBackend::Qsv, "hevc_qsv", "p010le", OutputLayout::P010, "preset=veryslow|global_quality={q}|profile=main10", 22},
            // The vcpkg x265 is 8 bit only: the CPU fallback of this preset
            // writes 8 bit 4:2:0 (P010 truncated by swscale).
            {VideoPreset::HevcNvenc, EncoderBackend::Cpu, "libx265", "yuv420p", OutputLayout::P010, "preset=medium|crf={q}", 20},
            // AV1 10 bit
            {VideoPreset::Av1Nvenc, EncoderBackend::Nvenc, "av1_nvenc", "p010le", OutputLayout::P010, "preset=p6|tune=hq|rc=vbr|b=0|cq={q}", 24},
            {VideoPreset::Av1Nvenc, EncoderBackend::Amf, "av1_amf", "p010le", OutputLayout::P010, "usage=transcoding|quality=quality|rc=cqp|qp_i={q}|qp_p={q}", 100},
            {VideoPreset::Av1Nvenc, EncoderBackend::Qsv, "av1_qsv", "p010le", OutputLayout::P010, "preset=veryslow|global_quality={q}", 24},
            {VideoPreset::Av1Nvenc, EncoderBackend::Cpu, "libaom-av1", "yuv420p10le", OutputLayout::P010, "cpu-used=6|row-mt=1|tiles=2x2|crf={q}|b=0", 28},
            // Preview: H.264, half size (scaled in the filter chain)
            {VideoPreset::PreviewH264, EncoderBackend::Nvenc, "h264_nvenc", "nv12", OutputLayout::Nv12, "preset=p4|rc=vbr|b=0|profile=high|cq={q}", 27},
            {VideoPreset::PreviewH264, EncoderBackend::Amf, "h264_amf", "nv12", OutputLayout::Nv12, "usage=transcoding|quality=speed|rc=cqp|qp_i={q}|qp_p={q}|qp_b={q}", 27},
            {VideoPreset::PreviewH264, EncoderBackend::Qsv, "h264_qsv", "nv12", OutputLayout::Nv12, "preset=fast|global_quality={q}", 27},
            {VideoPreset::PreviewH264, EncoderBackend::Cpu, "libx264", "nv12", OutputLayout::Nv12, "preset=veryfast|crf={q}", 26},
            // Lossless H.265 4:4:4
            {VideoPreset::LosslessNvencHevc, EncoderBackend::Nvenc, "hevc_nvenc", "yuv444p", OutputLayout::Rgba8, "preset=p7|tune=lossless", 0},
            {VideoPreset::LosslessNvencHevc, EncoderBackend::Cpu, "libx265", "yuv444p", OutputLayout::Rgba8, "preset=ultrafast|x265-params=lossless=1", 0},
            // Masters (CPU, native FFmpeg encoders)
            {VideoPreset::ProRes422Hq, EncoderBackend::Cpu, "prores_ks", "yuv422p10le", OutputLayout::X2Bgr10, "profile=3|vendor=apl0", 0},
            {VideoPreset::DnxhrHqx, EncoderBackend::Cpu, "dnxhd", "yuv422p10le", OutputLayout::X2Bgr10, "profile=dnxhr_hqx", 0},
            {VideoPreset::LosslessFfv1, EncoderBackend::Cpu, "ffv1", "bgr0", OutputLayout::Rgba8, "level=3|g=1|slices=16|slicecrc=1", 0},
            {VideoPreset::PngSequence, EncoderBackend::Cpu, "png", "rgb24", OutputLayout::Rgba8, "", 0},
            // AV1 on the CPU only (not in the dialog: the AV1 preset falls back to it)
            {VideoPreset::Av1Svt, EncoderBackend::Cpu, "libaom-av1", "yuv420p10le", OutputLayout::P010, "cpu-used=6|row-mt=1|tiles=2x2|crf={q}|b=0", 28},
        };
        return variants;
    }

    inline std::vector<std::size_t> VariantsOf(VideoPreset preset)
    {
        std::vector<std::size_t> out;
        const auto& all = AllVariants();
        for (std::size_t i = 0; i < all.size(); ++i)
            if (all[i].preset == preset)
                out.push_back(i);
        return out;
    }

    // What the probe found: one flag per AllVariants() entry.
    struct ProbeResult
    {
        bool done = false;
        bool runtimeLoaded = false;     // the DLLs of RedemptionEditor\dlls loaded
        std::string runtimeError;       // why not
        std::vector<bool> opened;       // per variant
        std::vector<std::string> notes; // per variant: the avcodec_open2 error, or ""
    };

    // The variant a preset runs with: the first one that opened (auto) or
    // the forced backend's; -1 when none.
    inline int ChooseVariant(VideoPreset preset, const ProbeResult& probe, bool automatic, EncoderBackend forced)
    {
        for (std::size_t index : VariantsOf(preset))
        {
            if (index >= probe.opened.size() || !probe.opened[index])
                continue;
            const EncoderVariant& v = AllVariants()[index];
            // A forced backend applies where the preset has it; the masters
            // only exist on the CPU and keep it.
            if (!automatic && v.backend != forced)
            {
                bool presetHasForced = false;
                for (std::size_t other : VariantsOf(preset))
                    presetHasForced = presetHasForced || AllVariants()[other].backend == forced;
                if (presetHasForced)
                    continue;
            }
            return static_cast<int>(index);
        }
        return -1;
    }

    // HDR10 keeps 10 bits: an NV12 variant moves to P010 (and the 8 bit
    // pixel formats to their 10 bit twins).
    inline OutputLayout VariantLayout(const EncoderVariant& v, bool hdr10) noexcept
    {
        return hdr10 && v.input == OutputLayout::Nv12 ? OutputLayout::P010 : v.input;
    }

    inline std::string VariantPixFmt(const EncoderVariant& v, bool hdr10)
    {
        const std::string fmt = v.pixFmt;
        if (!hdr10)
            return fmt;
        if (fmt == "nv12")
            return v.backend == EncoderBackend::Cpu ? "yuv420p10le" : "p010le";
        return fmt;
    }

    inline const char* LayoutPixFmt(OutputLayout layout) noexcept
    {
        switch (layout)
        {
        case OutputLayout::Nv12: return "nv12";
        case OutputLayout::P010: return "p010le";
        case OutputLayout::Rgba8: return "rgba";
        case OutputLayout::X2Bgr10: return "x2bgr10le";
        }
        return "rgba";
    }

    inline bool IsYuvPixFmt(const std::string& fmt)
    {
        return fmt.rfind("yuv", 0) == 0 || fmt == "nv12" || fmt.rfind("p010", 0) == 0;
    }

    // The options with {q} filled in: the job's quality, or the variant's.
    inline std::vector<std::pair<std::string, std::string>> VariantOptions(const EncoderVariant& v, std::uint32_t quality)
    {
        std::vector<std::pair<std::string, std::string>> out;
        const std::string q = std::to_string(quality ? quality : v.defaultQuality);
        std::string text = v.options;
        std::size_t start = 0;
        while (start < text.size())
        {
            std::size_t end = text.find('|', start);
            if (end == std::string::npos)
                end = text.size();
            const std::string pair = text.substr(start, end - start);
            const std::size_t eq = pair.find('=');
            if (eq != std::string::npos)
            {
                std::string value = pair.substr(eq + 1);
                for (std::size_t at = value.find("{q}"); at != std::string::npos; at = value.find("{q}"))
                    value.replace(at, 3, q);
                out.emplace_back(pair.substr(0, eq), value);
            }
            start = end + 1;
        }
        return out;
    }

    // libavfilter path for a .cube file (lut3d=file=...): forward slashes,
    // the drive colon and quotes escaped, the value in single quotes.
    inline std::string FilterPathUtf8(const std::string& path)
    {
        std::string out;
        for (char c : path)
        {
            if (c == '\\')
                out += '/';
            else if (c == ':')
                out += "\\:";
            else if (c == '\'')
                out += "\\'";
            else
                out += c;
        }
        return "'" + out + "'";
    }

    // The libavfilter chain between the GPU frame and the encoder, empty
    // when the frame goes in as it is (the NVENC presets without a LUT: no
    // CPU work at all). `lutUtf8` is the .cube path, empty for none.
    inline std::string VideoFilterChain(VideoPreset preset, OutputLayout layout, const std::string& encoderPixFmt, bool hdr10,
        const std::string& lutUtf8)
    {
        std::vector<std::string> filters;
        const bool yuvInput = layout == OutputLayout::Nv12 || layout == OutputLayout::P010;
        const bool yuvOutput = IsYuvPixFmt(encoderPixFmt);
        const char* matrix = hdr10 ? "bt2020" : "bt709";
        bool converted = false;
        if (!lutUtf8.empty())
        {
            if (yuvInput)
                filters.push_back(std::string("scale=in_color_matrix=") + matrix + ":in_range=tv");
            // lut3d works on packed RGB; the scale above picks gbrp/rgb48 for it.
            filters.push_back("lut3d=file=" + FilterPathUtf8(lutUtf8));
            converted = true;
        }
        if (preset == VideoPreset::PreviewH264)
            filters.push_back("scale=trunc(iw/4)*2:trunc(ih/4)*2:flags=bicubic");
        if (yuvOutput && (!yuvInput || converted))
            filters.push_back(std::string("scale=out_color_matrix=") + matrix + ":out_range=tv");
        if (!filters.empty() || encoderPixFmt != LayoutPixFmt(layout))
            filters.push_back("format=" + encoderPixFmt);
        std::string chain;
        for (std::size_t i = 0; i < filters.size(); ++i)
            chain += (i ? "," : "") + filters[i];
        return chain;
    }

    // The muxer for a final file name (avformat_alloc_output_context2).
    inline const char* MuxerFor(const std::wstring& path, VideoPreset preset)
    {
        if (preset == VideoPreset::PngSequence)
            return "image2";
        auto ends = [&path](const wchar_t* ext) {
            const std::size_t n = std::char_traits<wchar_t>::length(ext);
            return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
        };
        if (ends(L".mp4"))
            return "mp4";
        if (ends(L".mov"))
            return "mov";
        return "matroska";
    }

    // The video pass's file must stay playable when the game dies mid export: a
    // plain MP4 writes its moov index only in av_write_trailer, so a cut file
    // opens nowhere.
    // MP4 and MOV: fragmented. An empty moov goes first, then a moof + mdat pair
    // per fragment (delay_moov writes that moov with the first fragment, once the
    // first packets are known; without it DNxHD's ACLR range atom is missing). A
    // file cut anywhere decodes up to its last whole fragment. default_base_moof:
    // offsets relative to each moof (what browsers, QuickTime, ffmpeg and Media
    // Foundation expect).
    //   long GOP: a fragment at each keyframe and at least every second (NVENC's
    //     GOP is 250 frames, 4 s at 60 fps, too much to lose);
    //   intra only (ProRes, DNxHR): a fragment per second, not one per frame;
    //   both: at most 64 MB per fragment, which caps libavformat's buffered
    //     memory and the loss for very high bit rates.
    // MKV (FFV1, lossless HEVC): one cluster per second at most; Matroska needs no
    // index to play. The fragmented file is only the temporary one: the finalize
    // remuxes it (video copied bit for bit) into a plain MP4 (+faststart) or MOV.

    constexpr std::uint64_t kFragmentBytes = 64ull << 20;
    constexpr std::uint64_t kFragmentUs = 1000000;

    inline bool IntraOnlyPreset(VideoPreset preset) noexcept
    {
        return preset == VideoPreset::ProRes422Hq || preset == VideoPreset::DnxhrHqx || preset == VideoPreset::LosslessFfv1 ||
            preset == VideoPreset::PngSequence;
    }

    inline bool FragmentedMuxer(const std::string& muxer) noexcept { return muxer == "mp4" || muxer == "mov"; }

    // The muxer options of the video pass (avformat_write_header's dictionary).
    inline std::vector<std::pair<std::string, std::string>> CrashSafeMuxOptions(const std::string& muxer, VideoPreset preset)
    {
        std::vector<std::pair<std::string, std::string>> out;
        if (FragmentedMuxer(muxer))
        {
            const bool intra = IntraOnlyPreset(preset);
            std::string flags = intra ? "+empty_moov+delay_moov+default_base_moof" : "+frag_keyframe+empty_moov+delay_moov+default_base_moof";
            if (muxer == "mov")
                flags += "+write_colr";
            out.emplace_back("movflags", flags);
            out.emplace_back("frag_duration", std::to_string(kFragmentUs));
            out.emplace_back("frag_size", std::to_string(kFragmentBytes));
        }
        else if (muxer == "matroska")
        {
            out.emplace_back("cluster_time_limit", "1000");
        }
        return out;
    }

    // The same for the ffmpeg.exe fallback backend's command line (output
    // options, before the output name).
    inline std::wstring CrashSafeMuxArgs(const std::wstring& path, VideoPreset preset)
    {
        std::wstring args;
        for (const auto& option : CrashSafeMuxOptions(MuxerFor(path, preset), preset))
            args += L" -" + std::wstring(option.first.begin(), option.first.end()) + L" " + std::wstring(option.second.begin(), option.second.end());
        return args;
    }

    // Audio codec of the finalize (same as the ffmpeg.exe finalize).
    inline const char* FinalAudioCodec(const std::wstring& path)
    {
        auto ends = [&path](const wchar_t* ext) {
            const std::size_t n = std::char_traits<wchar_t>::length(ext);
            return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
        };
        if (ends(L".mov"))
            return "pcm_s24le";
        if (ends(L".mp4"))
            return "aac";
        return "flac";
    }
}
