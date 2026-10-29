#pragma once

// Render export: types shared by the add-on side modules. Pure data; no
// ReShade or Windows types here so the core tests can include it.

#include "convert_ref.h"
#include "export_channel.h"
#include "export_schedule.h"

#include <cstdint>
#include <string>
#include <vector>

namespace render_export
{
    // Pixel layout of a frame handed to the encoder (what the GPU resolve
    // wrote and the readback copied).
    enum class InputPixels : std::uint32_t
    {
        Rgba8 = 0,     // R8G8B8A8_UNORM, sRGB encoded (SDR)
        Bgra8 = 1,     // B8G8R8A8_UNORM
        Rgb10a2 = 2,   // R10G10B10A2_UNORM (SDR 10 bit, or HDR10 PQ)
        Rgba16f = 3    // R16G16B16A16_FLOAT (scRGB linear, HDR)
    };

    constexpr std::uint32_t BytesPerPixel(InputPixels pixels) noexcept
    {
        return pixels == InputPixels::Rgba16f ? 8u : 4u;
    }

    enum class ExportError : std::uint32_t
    {
        None = 0,
        ChannelMissing = 1,     // no probe channel (probe too old or export disabled)
        TimerNotLocked = 2,     // the probe could not fix the game timer: refused (the video would stutter)
        UnsupportedFormat = 3,  // back buffer format not handled
        SizeChanged = 4,        // the back buffer changed size during the export
        AccumulatorFailed = 5,  // the accumulation effect or pipeline is missing
        EncoderStart = 6,       // the encoder did not open (no variant opened, or ffmpeg.exe did not start)
        EncoderWrite = 7,       // encode or write failed (disk full, bad option, ffmpeg.exe closed its input)
        EncoderExit = 8,        // the trailer failed (or ffmpeg.exe exited with an error code)
        LatencyUnknown = 9,     // the preroll tags were never read
        LatencySlipped = 10,    // the flush tags disagree with the preroll ones (a frame was lost)
        StampMissing = 11,      // a present without its tick stamp (the add-on fell behind the ring)
        Cancelled = 12,
        AudioCapture = 13,
        UnsupportedReShade = 14, // a ReShade whose get_query_heap_results ABI is not known (reshade_query_abi.h)
        GpuTimeout = 15,         // a GPU frame was not done after kDrainWaitLimitMs (gpu_export.h)
        AudioNotReady = 16,      // the probe got no audio capture acknowledgement in time (export_channel.h AudioAck)
        FinalizeFailed = 17,     // the video could not be saved under the requested name (it stays under its temporary one)
        ProjectClipMissing = 18, // whole project export: a clip of the project is not in ReplayClips (refused before anything ran)
        ProjectClipLoad = 19     // whole project export: a clip did not open (or the project changed): stopped, the pieces deleted
    };

    // One output frame in CPU memory, ready for the encoder.
    struct VideoFrame
    {
        std::int64_t index = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        InputPixels pixels = InputPixels::Rgba8;
        std::vector<std::uint8_t> data; // tightly packed rows (width * bpp)
    };

    // Everything the encoder needs, built from exportchannel::Settings and
    // what the capture found (size, colour space).
    struct EncodeJob
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        replay::exporting::FrameRate rate;
        OutputLayout layout = OutputLayout::Nv12; // what the GPU resolve writes (LayoutFor)
        replay::exportchannel::VideoPreset preset = replay::exportchannel::VideoPreset::H264Nvenc;
        std::uint32_t quality = 0; // 0: the preset's default
        bool hdr10 = false;        // input is PQ BT.2020 and the output keeps it
        std::wstring ffmpeg = L"ffmpeg.exe"; // the ffmpeg.exe fallback backend only
        // In process encoder (libav_encoder.h): the variant the probe chose
        // (encoder_select.h AllVariants index, -1: the first that opens) and
        // the backend forced by [Export] Encoder (automatic = best first).
        int variant = -1;
        bool automaticBackend = true;
        std::uint32_t forcedBackend = 0;
        // The JPEG thumbnail, taken from this frame on its way to the
        // encoder (no decode of the finished file); empty = none.
        std::wstring thumbnail;
        std::uint64_t thumbnailFrame = 0;
        std::wstring output;       // the video pass's file (temporary; a folder for an image sequence)
        std::wstring finalOutput;  // the requested output file (after the finalize remux)
        std::wstring lut;          // .cube applied by ffmpeg (lut3d); empty = none
        std::wstring audioWav;     // the audio pass (empty: silent)
        std::wstring chapters;     // FFMETADATA file (empty: none)
    };
}
