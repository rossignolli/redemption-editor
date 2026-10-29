#pragma once

// Render export: the presets (InfoOf: container, label, GPU layout, size
// estimate, default file name) and the ffmpeg.exe command lines of the
// optional fallback backend ([Export] Backend=ffmpeg, FfmpegPipeSink). The
// default backend links the FFmpeg libraries in process like EVER
// (libav_encoder.h; the encoder per preset is chosen in encoder_select.h).
//
// The frames arrive already in the encoder's layout, converted on the GPU
// by shaders/export_convert.hlsl (convert_ref.h): NV12 or P010 in BT.709
// limited range for the NVENC presets (NVENC takes them as they are, no
// swscale on the CPU), packed RGB for the RGB masters. Every YUV output is
// tagged (primaries, transfer, matrix, range): EVER left the matrix
// untagged and players guess.
//
// The video pass writes a temporary file; the finalize step (audio from the
// audio pass, chapters from the markers) remuxes it into the final name
// with the video copied bit for bit.
// Pure (unit tested in ReplayCoreTests).

#include "convert_ref.h"
#include "encoder_select.h" // CrashSafeMuxArgs
#include "export_types.h"
#include "i18n.h" // TR_NOOP: the dialog's texts and their keys

#include <cstdio>
#include <string>
#include <vector>

namespace render_export
{
    using replay::exportchannel::VideoPreset;

    struct PresetInfo
    {
        const wchar_t* extension;  // container of the final file
        const char* labelKey;      // the label's lang key (TR(labelKey, label) in the dialog)
        const char* label;         // dialog label (English)
        const char* noteKey;
        const char* note;          // one line under the label
        OutputLayout layout;       // what the GPU resolve writes
        bool gpuEncoder;           // a GPU encoder first (NVENC, AMF, QSV); the CPU otherwise
        bool available;            // in an ffmpeg.exe essentials build (fallback backend; no libsvtav1)
        double bitsPerPixel;       // size estimate per frame pixel (game footage, rough)
    };

    inline PresetInfo InfoOf(VideoPreset preset) noexcept
    {
        switch (preset)
        {
        case VideoPreset::H264Nvenc:
            return {L".mp4", TR_NOOP("export.preset.h264", "H.264"), TR_NOOP("export.preset.h264.note", "YouTube, any player."), OutputLayout::Nv12, true, true, 0.20};
        case VideoPreset::HevcNvenc:
            return {L".mp4", TR_NOOP("export.preset.h265", "H.265 10-bit"), TR_NOOP("export.preset.h265.note", "YouTube 4K60, smaller file."), OutputLayout::P010, true, true, 0.10};
        case VideoPreset::Av1Nvenc:
            return {L".mp4", TR_NOOP("export.preset.av1", "AV1 10-bit"), TR_NOOP("export.preset.av1.note", "Smallest delivery file (GPU: RTX 40/50, Radeon RX 7000, Arc)."), OutputLayout::P010, true, true, 0.08};
        case VideoPreset::LosslessNvencHevc:
            return {L".mkv", TR_NOOP("export.preset.h265_lossless", "H.265 lossless 4:4:4"), TR_NOOP("export.preset.h265_lossless.note", "Lossless, big file."), OutputLayout::Rgba8, true, true, 7.0};
        case VideoPreset::LosslessFfv1:
            return {L".mkv", TR_NOOP("export.preset.ffv1", "FFV1 lossless (master)"), TR_NOOP("export.preset.ffv1.note", "Exact RGB, for editing. Very big file."), OutputLayout::Rgba8, false, true, 12.0};
        case VideoPreset::ProRes422Hq:
            return {L".mov", TR_NOOP("export.preset.prores", "ProRes 422 HQ (master)"), TR_NOOP("export.preset.prores.note", "Premiere / Resolve intermediate. CPU."), OutputLayout::X2Bgr10, false, true, 3.6};
        case VideoPreset::DnxhrHqx:
            return {L".mov", TR_NOOP("export.preset.dnxhr", "DNxHR HQX 10-bit (master)"), TR_NOOP("export.preset.dnxhr.note", "Avid / Premiere intermediate. CPU."), OutputLayout::X2Bgr10, false, true, 3.5};
        case VideoPreset::PngSequence:
            return {L"", TR_NOOP("export.preset.png", "PNG sequence"), TR_NOOP("export.preset.png.note", "One image per frame in a folder."), OutputLayout::Rgba8, false, true, 12.0};
        case VideoPreset::Av1Svt:
            return {L".mp4", TR_NOOP("export.preset.av1_cpu", "AV1 10-bit (CPU)"), TR_NOOP("export.preset.av1_cpu.note", "Software AV1 (libaom): slow."), OutputLayout::P010, false, false, 0.07};
        case VideoPreset::PreviewH264:
            return {L".mp4", TR_NOOP("export.preset.preview", "Preview (H.264, half size)"), TR_NOOP("export.preset.preview.note", "Quick look, small file."), OutputLayout::Nv12, true, true, 0.08};
        }
        return {L".mkv", "", "?", "", "", OutputLayout::Rgba8, false, false, 12.0};
    }

    // HDR10 keeps 10 bits: NV12 presets move to P010.
    inline OutputLayout LayoutFor(VideoPreset preset, bool hdr10) noexcept
    {
        const OutputLayout layout = InfoOf(preset).layout;
        return hdr10 && layout == OutputLayout::Nv12 ? OutputLayout::P010 : layout;
    }

    inline const wchar_t* FfmpegPixelName(OutputLayout layout) noexcept
    {
        switch (layout)
        {
        case OutputLayout::Nv12: return L"nv12";
        case OutputLayout::P010: return L"p010le";
        case OutputLayout::Rgba8: return L"rgba";
        case OutputLayout::X2Bgr10: return L"x2bgr10le";
        }
        return L"rgba";
    }

    inline std::wstring Quote(const std::wstring& path)
    {
        return L"\"" + path + L"\"";
    }

    inline std::wstring Number(std::uint64_t value)
    {
        return std::to_wstring(value);
    }

    // Path for ffmpeg filter arguments (lut3d=file=...): forward slashes,
    // the drive colon escaped, the whole value in single quotes.
    inline std::wstring FilterPath(const std::wstring& path)
    {
        std::wstring out;
        for (wchar_t c : path)
        {
            if (c == L'\\')
                out += L'/';
            else if (c == L':')
                out += L"\\:";
            else if (c == L'\'')
                out += L"\\'";
            else
                out += c;
        }
        return L"'" + out + L"'";
    }

    // Output stream tags, only for RGB input converted by swscale. NV12 and
    // P010 frames are tagged on the input (before -i): repeating the tags on
    // the output makes ffmpeg 7.1 insert a CPU conversion pass (h264_nvenc
    // 1080p from 145 to 59 fps).
    inline std::wstring ColourTags(bool hdr10, OutputLayout layout)
    {
        if (layout == OutputLayout::Nv12 || layout == OutputLayout::P010)
            return {};
        if (hdr10)
            return L" -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -color_range tv";
        return L" -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv";
    }

    // The -vf chain: LUT (needs RGB: ffmpeg converts around it), preview
    // scale, and for RGB input to a YUV codec the matrix conversion.
    inline std::wstring VideoFilters(const EncodeJob& job, OutputLayout layout, bool yuvOutput)
    {
        std::vector<std::wstring> filters;
        const bool yuvInput = layout == OutputLayout::Nv12 || layout == OutputLayout::P010;
        if (!job.lut.empty())
        {
            if (yuvInput)
                filters.push_back(job.hdr10 ? L"scale=in_color_matrix=bt2020:in_range=tv" : L"scale=in_color_matrix=bt709:in_range=tv");
            filters.push_back(L"lut3d=file=" + FilterPath(job.lut));
        }
        if (job.preset == VideoPreset::PreviewH264)
            filters.push_back(L"scale=trunc(iw/4)*2:trunc(ih/4)*2:flags=bicubic");
        if (yuvOutput && (!yuvInput || !job.lut.empty() || job.preset == VideoPreset::PreviewH264))
            filters.push_back(job.hdr10 ? L"scale=out_color_matrix=bt2020:out_range=tv" : L"scale=out_color_matrix=bt709:out_range=tv");
        if (filters.empty())
            return {};
        std::wstring chain = L" -vf ";
        for (std::size_t i = 0; i < filters.size(); ++i)
            chain += (i ? L"," : L"") + filters[i];
        return chain;
    }

    // Arguments after "ffmpeg.exe" for the video pass (raw frames on stdin
    // into job.output, which is the temporary video file).
    inline std::wstring BuildFfmpegArgs(const EncodeJob& job)
    {
        const OutputLayout layout = job.layout;
        std::wstring args = L"-hide_banner -loglevel warning -nostats -y";
        args += L" -f rawvideo -pix_fmt ";
        args += FfmpegPixelName(layout);
        args += L" -s " + Number(job.width) + L"x" + Number(job.height);
        args += L" -r " + Number(job.rate.num) + L"/" + Number(job.rate.den);
        if (layout == OutputLayout::Nv12 || layout == OutputLayout::P010)
            args += job.hdr10 ? L" -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -color_range tv"
                              : L" -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv";
        args += L" -i -";
        const std::uint32_t q = job.quality;
        const bool tenBitIn = layout == OutputLayout::P010 || layout == OutputLayout::X2Bgr10;
        switch (job.preset)
        {
        case VideoPreset::LosslessFfv1:
            args += VideoFilters(job, layout, false);
            // 8 bit RGB is bgr0 in FFV1 (gbrp is refused for 8 bit: checked
            // with the essentials 7.1 build), 10 bit planar gbrp10le.
            args += tenBitIn ? L" -c:v ffv1 -level 3 -g 1 -slices 16 -slicecrc 1 -pix_fmt gbrp10le"
                             : L" -c:v ffv1 -level 3 -g 1 -slices 16 -slicecrc 1 -pix_fmt bgr0";
            break;
        case VideoPreset::LosslessNvencHevc:
            args += VideoFilters(job, layout, true);
            args += L" -c:v hevc_nvenc -preset p7 -tune lossless -pix_fmt yuv444p";
            args += ColourTags(job.hdr10, layout);
            break;
        case VideoPreset::Av1Nvenc:
            args += VideoFilters(job, layout, true);
            args += L" -c:v av1_nvenc -preset p6 -tune hq -rc vbr -b:v 0 -cq " + Number(q ? q : 24) + L" -pix_fmt p010le";
            args += ColourTags(job.hdr10, layout);
            break;
        case VideoPreset::HevcNvenc:
            args += VideoFilters(job, layout, true);
            args += L" -c:v hevc_nvenc -preset p6 -tune hq -rc vbr -b:v 0 -profile:v main10 -pix_fmt p010le -cq " + Number(q ? q : 20);
            args += ColourTags(job.hdr10, layout);
            break;
        case VideoPreset::H264Nvenc:
            args += VideoFilters(job, layout, true);
            args += L" -c:v h264_nvenc -preset p6 -tune hq -rc vbr -b:v 0 -profile:v high -pix_fmt yuv420p -cq " + Number(q ? q : 19);
            args += ColourTags(false, layout);
            break;
        case VideoPreset::PreviewH264:
            args += VideoFilters(job, layout, true);
            args += L" -c:v h264_nvenc -preset p4 -rc vbr -b:v 0 -profile:v high -pix_fmt yuv420p -cq " + Number(q ? q : 27);
            args += ColourTags(false, layout);
            break;
        case VideoPreset::ProRes422Hq:
            args += VideoFilters(job, layout, true);
            args += L" -c:v prores_ks -profile:v 3 -vendor apl0 -pix_fmt yuv422p10le";
            args += ColourTags(job.hdr10, layout); // +write_colr: CrashSafeMuxArgs (MOV)
            break;
        case VideoPreset::DnxhrHqx:
            args += VideoFilters(job, layout, true);
            args += L" -c:v dnxhd -profile:v dnxhr_hqx -pix_fmt yuv422p10le";
            args += ColourTags(job.hdr10, layout);
            break;
        case VideoPreset::PngSequence:
            args += VideoFilters(job, layout, false);
            args += tenBitIn ? L" -c:v png -pix_fmt rgb48be" : L" -c:v png -pix_fmt rgb24";
            args += L" -f image2";
            break;
        case VideoPreset::Av1Svt:
            args += VideoFilters(job, layout, true);
            args += L" -c:v libsvtav1 -preset 6 -pix_fmt yuv420p10le -crf " + Number(q ? q : 24);
            args += ColourTags(job.hdr10, layout);
            break;
        }
        if (job.preset == VideoPreset::PngSequence)
            args += L" " + Quote(job.output + L"\\%06d.png");
        else
            args += CrashSafeMuxArgs(job.output, job.preset) + L" " + Quote(job.output);
        return args;
    }

    // ffmpeg's FFMETADATA chapters from the markers (ms from the export's
    // start). A chapter runs to the next one; the last to the end.
    struct Chapter
    {
        std::uint64_t startMs = 0;
        std::string title;
    };

    inline std::string BuildChapterMetadata(const std::vector<Chapter>& chapters, std::uint64_t durationMs)
    {
        std::string out = ";FFMETADATA1\n";
        for (std::size_t i = 0; i < chapters.size(); ++i)
        {
            const std::uint64_t start = chapters[i].startMs;
            const std::uint64_t end = i + 1 < chapters.size() ? chapters[i + 1].startMs : durationMs;
            if (end <= start)
                continue;
            out += "[CHAPTER]\nTIMEBASE=1/1000\nSTART=" + std::to_string(start) + "\nEND=" + std::to_string(end) + "\ntitle=";
            for (char c : chapters[i].title)
            {
                if (c == '=' || c == ';' || c == '#' || c == '\\' || c == '\n')
                    out += '\\';
                out += c;
            }
            out += '\n';
        }
        return out;
    }

    // Finalize: the temporary video plus (optional) the audio pass WAV and
    // (optional) the chapters, video copied, into the final file.
    // audioStartS: where the export's frame 0 sits in the WAV.
    // concatList: `videoPath` is an ffconcat list of a whole project's pieces
    // (BuildConcatList), read by the concat demuxer and copied as one video.
    inline std::wstring BuildFinalizeArgs(const EncodeJob& job, const std::wstring& videoPath, double audioStartS, bool concatList = false)
    {
        std::wstring args = L"-hide_banner -loglevel warning -nostats -y";
        if (concatList)
            args += L" -f concat -safe 0";
        args += L" -i " + Quote(videoPath);
        int next = 1;
        int audioInput = -1;
        int chapterInput = -1;
        if (!job.audioWav.empty())
        {
            if (audioStartS > 0.0)
            {
                wchar_t seek[32];
                std::swprintf(seek, 32, L" -ss %.6f", audioStartS);
                args += seek;
            }
            args += L" -i " + Quote(job.audioWav);
            audioInput = next++;
        }
        if (!job.chapters.empty())
        {
            args += L" -f ffmetadata -i " + Quote(job.chapters);
            chapterInput = next++;
        }
        args += L" -map 0:v:0";
        if (audioInput >= 0)
            args += L" -map " + Number(static_cast<std::uint64_t>(audioInput)) + L":a:0";
        if (chapterInput >= 0)
            args += L" -map_chapters " + Number(static_cast<std::uint64_t>(chapterInput));
        args += L" -c:v copy";
        if (audioInput >= 0)
        {
            const std::wstring& out = job.finalOutput;
            const bool mov = out.size() > 4 && out.compare(out.size() - 4, 4, L".mov") == 0;
            const bool mp4 = out.size() > 4 && out.compare(out.size() - 4, 4, L".mp4") == 0;
            if (mov)
                args += L" -c:a pcm_s24le";
            else if (mp4)
                args += L" -c:a aac -b:a 320k";
            else
                args += L" -c:a flac";
            args += L" -shortest";
        }
        const std::wstring& out = job.finalOutput;
        if (out.size() > 4 && out.compare(out.size() - 4, 4, L".mp4") == 0)
            args += L" -movflags +faststart";
        args += L" " + Quote(job.finalOutput);
        return args;
    }

    // The concat demuxer's list of a whole project's pieces (UTF-8): each
    // file in single quotes (a quote in a name written '\'').
    inline std::string BuildConcatList(const std::vector<std::string>& utf8Paths)
    {
        std::string out = "ffconcat version 1.0\n";
        for (const std::string& path : utf8Paths)
        {
            out += "file '";
            for (char c : path)
                out += c == '\'' ? std::string("'\\''") : std::string(1, c);
            out += "'\n";
        }
        return out;
    }

    // A JPEG from the frame at `atS` of the finished video.
    inline std::wstring BuildThumbnailArgs(const std::wstring& videoPath, double atS, const std::wstring& jpegPath)
    {
        wchar_t seek[32];
        std::swprintf(seek, 32, L"%.3f", atS > 0.0 ? atS : 0.0);
        return L"-hide_banner -loglevel warning -nostats -y -ss " + std::wstring(seek) + L" -i " + Quote(videoPath) +
            L" -frames:v 1 -update 1 -vf scale=1280:-2:out_range=pc,format=yuvj420p -q:v 3 " + Quote(jpegPath); // JPEG is full range
    }

    // Rough size of the file (bytes): bits per pixel of game footage per
    // preset (InfoOf), shown in the dialog as an estimate.
    inline std::uint64_t EstimateBytes(VideoPreset preset, std::uint32_t width, std::uint32_t height, replay::exporting::FrameRate rate,
        double seconds)
    {
        if (!replay::exporting::ValidRate(rate) || seconds <= 0.0)
            return 0;
        double pixels = static_cast<double>(width) * height;
        if (preset == VideoPreset::PreviewH264)
            pixels /= 4.0;
        const double frames = seconds * rate.num / rate.den;
        return static_cast<std::uint64_t>(pixels * frames * InfoOf(preset).bitsPerPixel / 8.0);
    }

    // Default file name: <folder>\<clip>_<yyyymmdd-hhmmss>_<preset tag><ext>.
    inline std::wstring DefaultOutputPath(const std::wstring& folder, const std::wstring& clip, VideoPreset preset, int year, int month,
        int day, int hour, int minute, int second)
    {
        static const wchar_t* const tags[] = {L"ffv1", L"hevc-lossless", L"av1", L"hevc", L"h264", L"prores", L"png", L"av1-svt",
            L"dnxhr", L"preview"};
        const std::uint32_t index = static_cast<std::uint32_t>(preset);
        std::wstring name;
        for (wchar_t c : clip.empty() ? std::wstring(L"clip") : clip)
            name += (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') ? L'_' : c;
        wchar_t stamp[32];
        std::swprintf(stamp, 32, L"_%04d%02d%02d-%02d%02d%02d_", year, month, day, hour, minute, second);
        std::wstring path = folder;
        if (!path.empty() && path.back() != L'\\')
            path += L'\\';
        return path + name + stamp + (index < 10 ? tags[index] : L"video") + InfoOf(preset).extension;
    }
}
