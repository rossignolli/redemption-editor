// Render export: the export clock (export_schedule.h), the shared stamp ring
// (export_channel.h), the encoder command line
// (editor/render_export/ffmpeg_args.h) and the in process encoder's choice per
// preset (editor/render_export/encoder_select.h).
#include "export_channel.h"
#include "export_schedule.h"
#include "export_timer.h"
#include "../editor/render_export/convert_ref.h"
#include "../editor/render_export/encoder_select.h"
#include "../editor/render_export/export_finish.h"
#include "../editor/render_export/ffmpeg_args.h"
#include "../editor/render_export/readback_ring.h"
#include "../editor/render_export/reshade_query_abi.h"
#include "../editor/readback_release.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>

namespace
{
    using namespace replay::exporting;

    int g_exportFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_exportFailures;
    }

    void TestFrameStart()
    {
        const FrameRate r60{60, 1};
        Check(FrameStartNs(r60, 0) == 0, "frame 0 starts at 0");
        Check(FrameStartNs(r60, 60) == kNsPerSecond, "60 frames at 60 fps are exactly one second");
        Check(FrameStartNs(r60, 1) == 16666666, "one 60 fps frame is 16666666 ns (floor)");
        const FrameRate ntsc{30000, 1001};
        Check(FrameStartNs(ntsc, 30000) == 1001ll * kNsPerSecond, "30000 frames at 29.97 are exactly 1001 s (no drift)");
        Check(FrameStartNs(ntsc, 3000000) == 100100ll * kNsPerSecond, "3 million frames at 29.97 do not overflow or drift");
        Check(FrameStartNs(r60, -30) == -500000000ll, "preroll frames are negative");
        Check(!ValidRate({0, 1}) && !ValidRate({60, 0}), "a zero rate is refused");
    }

    void TestSampleTimes()
    {
        const FrameRate r30{30, 1};
        Shutter shutter{1, 180.0f};
        Check(SampleTimeNs(r30, shutter, 3, 0) == FrameStartNs(r30, 3), "one sample is the frame instant");
        shutter = {4, 180.0f};
        const std::int64_t open = OpenNs(r30, shutter, 0);
        Check(open == 16666667, "180 degrees at 30 fps is half the frame interval");
        // Centred, midpoint rule: -3/8, -1/8, +1/8, +3/8 of the open interval.
        const std::int64_t s0 = SampleTimeNs(r30, shutter, 0, 0);
        const std::int64_t s3 = SampleTimeNs(r30, shutter, 0, 3);
        Check(std::llabs(s0 + open * 3 / 8) <= 1, "first sample 3/8 of the open interval before the instant");
        Check(std::llabs(s3 - open * 3 / 8) <= 1, "last sample 3/8 after");
        shutter = {8, 360.0f};
        const std::int64_t a = SampleTimeNs(r30, shutter, 5, 7);
        const std::int64_t b = SampleTimeNs(r30, shutter, 6, 0);
        const std::int64_t step = SampleTimeNs(r30, shutter, 5, 1) - SampleTimeNs(r30, shutter, 5, 0);
        Check(std::llabs((b - a) - step) <= 1, "360 degrees: evenly spaced across frames (continuous blur)");
        shutter = {8, 0.0f};
        Check(SampleTimeNs(r30, shutter, 2, 5) == FrameStartNs(r30, 2), "0 degrees: every sample at the instant");
    }

    void TestClockCapture()
    {
        Settings settings;
        settings.rate = {60, 1};
        settings.shutter = {4, 180.0f};
        settings.prerollFrames = 3;
        settings.flushTicks = 2;
        settings.maxFrames = 10;
        Clock clock;
        clock.Begin(settings);
        int preroll = 0, capture = 0, flush = 0, first = 0, last = 0;
        float weightSum = 0.0f;
        std::int64_t stepSum = 0;
        std::uint64_t serial = 0;
        bool serialOk = true;
        std::uint64_t transportBefore = clock.TransportNowMs(1000);
        bool transportMonotonic = true;
        std::int64_t firstCaptureTime = 0;
        std::int64_t lastCaptureTime = 0;
        for (int i = 0; i < 1000 && clock.Active(); ++i)
        {
            const Tick tick = clock.Next();
            serialOk = serialOk && tick.serial == serial++;
            const std::uint64_t transportNow = clock.TransportNowMs(1000);
            transportMonotonic = transportMonotonic && transportNow >= transportBefore;
            transportBefore = transportNow;
            if (tick.phase == Phase::Preroll)
                ++preroll;
            else if (tick.phase == Phase::Capture)
            {
                if (capture == 0)
                    firstCaptureTime = tick.timeNs;
                else
                    stepSum += tick.stepNs;
                lastCaptureTime = tick.timeNs;
                ++capture;
                first += tick.firstOfFrame ? 1 : 0;
                last += tick.lastOfFrame ? 1 : 0;
                weightSum += tick.weight;
            }
            else if (tick.phase == Phase::Flush)
                ++flush;
            Check(tick.stepNs > 0, "every tick moves time forward");
        }
        Check(!clock.Active() && clock.CurrentPhase() == Phase::Done, "the clock ends");
        Check(preroll == 3, "three preroll ticks");
        Check(capture == 40, "10 frames x 4 samples");
        Check(first == 10 && last == 10, "a first and a last sample per frame");
        Check(std::fabs(weightSum - 10.0f) < 1e-3f, "the weights of a frame add up to 1");
        Check(flush == 2, "two flush ticks");
        Check(serialOk, "serials count ticks");
        Check(transportMonotonic, "the transport clock never goes back");
        Check(stepSum == lastCaptureTime - firstCaptureTime, "the steps add up to the elapsed output time (no drift)");
        Check(clock.FramesEmitted() == 10, "10 frames emitted");
    }

    void TestClockFinish()
    {
        Settings settings;
        settings.rate = {30, 1};
        settings.shutter = {3, 270.0f};
        settings.prerollFrames = 0;
        settings.flushTicks = 1;
        Clock clock;
        clock.Begin(settings);
        Tick tick = clock.Next();
        Check(tick.phase == Phase::Capture && tick.firstOfFrame, "no preroll: the first tick captures");
        clock.Finish(); // the end is reached in the middle of frame 0
        tick = clock.Next();
        Check(tick.phase == Phase::Capture && tick.sample == 1, "the frame being sampled completes");
        tick = clock.Next();
        Check(tick.lastOfFrame, "its last sample");
        tick = clock.Next();
        Check(tick.phase == Phase::Flush, "then flush");
        tick = clock.Next();
        Check(tick.phase == Phase::Done, "then done");
        Check(clock.FramesEmitted() == 1, "one frame");
    }

    void TestClosedShutterStep()
    {
        // 180 degrees, 2 samples at 30 fps: the long step skips the closed
        // half of the interval in one tick (not rendered for nothing).
        Settings settings;
        settings.rate = {30, 1};
        settings.shutter = {2, 180.0f};
        settings.prerollFrames = 0;
        settings.maxFrames = 3;
        Clock clock;
        clock.Begin(settings);
        clock.Next(); // f0 s0
        const Tick inside = clock.Next(); // f0 s1
        const Tick across = clock.Next(); // f1 s0
        const std::int64_t frame = FrameStartNs(settings.rate, 1);
        Check(std::llabs(inside.stepNs - frame / 4) <= 1, "inside the open shutter the step is open/N");
        Check(std::llabs(across.stepNs - (frame - frame / 4)) <= 1, "across the closed part: interval - (N-1) open/N");
    }

    void TestTag()
    {
        Check(TagBit(0x5, 0) && !TagBit(0x5, 1) && TagBit(0x5, 2), "tag bits are the serial's bits");
        Check(LatencyFromTag(12, 10) == 2, "latency = presents - tagged serial");
        Check(LatencyFromTag(257, 255) == 2, "the tag wraps at 8 bits");
    }

    void TestStampRing()
    {
        static replay::exportchannel::Channel channel; // big: not on the stack
        Tick tick;
        tick.phase = Phase::Capture;
        tick.serial = 7;
        tick.frame = 1;
        tick.sample = 2;
        tick.samples = 4;
        tick.weight = 0.25f;
        tick.lastOfFrame = false;
        replay::exportchannel::WriteStamp(channel, tick, 1234, false);
        replay::exportchannel::TickStamp out;
        Check(replay::exportchannel::ReadStamp(channel, 7, out), "a written stamp is read");
        Check(out.frame == 1 && out.sample == 2 && out.playheadMs == 1234, "its fields");
        Check(!replay::exportchannel::ReadStamp(channel, 8, out), "a missing serial is refused");
        tick.serial = 7 + replay::exportchannel::kTickRing;
        replay::exportchannel::WriteStamp(channel, tick, 1, false);
        Check(!replay::exportchannel::ReadStamp(channel, 7, out), "an overwritten stamp is refused");
        Check(channel.probeSerial.load() == tick.serial + 1, "probeSerial follows");
    }

    void TestReadbackRing()
    {
        render_export::ReadbackRing<3> ring;
        Check(ring.Acquire(0) == 0 && ring.Acquire(1) == 1 && ring.Acquire(2) == 2, "three slots in flight");
        Check(ring.Acquire(3) == render_export::kNoSlot, "a fourth waits (no stall, no drop: the caller holds the present)");
        Check(ring.OldestReady() == render_export::kNoSlot, "nothing is mapped before the GPU finished");
        ring.MarkGpuDone(0);
        ring.MarkGpuDone(2);
        Check(ring.OldestReady() == 0, "frames leave in order");
        ring.Release(0);
        Check(ring.OldestReady() == render_export::kNoSlot, "slot 1 blocks slot 2 (order)");
        ring.MarkGpuDone(1);
        Check(ring.OldestReady() == 1, "then 1");
        Check(ring.Acquire(4) == 0, "a freed slot is reused");
    }

    void TestFfmpegArgs()
    {
        using replay::exportchannel::VideoPreset;
        render_export::EncodeJob job;
        job.width = 3840;
        job.height = 2160;
        job.rate = {60, 1};
        job.preset = VideoPreset::H264Nvenc;
        job.layout = render_export::LayoutFor(job.preset, false);
        job.output = L"D:\\videos\\x.video.mp4";
        std::wstring args = render_export::BuildFfmpegArgs(job);
        Check(job.layout == render_export::OutputLayout::Nv12, "H.264 takes NV12 from the GPU");
        Check(args.find(L"-f rawvideo -pix_fmt nv12") != std::wstring::npos, "raw NV12 frames on stdin");
        Check(args.find(L"-s 3840x2160") != std::wstring::npos && args.find(L"-r 60/1") != std::wstring::npos, "size and rational rate");
        Check(args.find(L"-c:v h264_nvenc") != std::wstring::npos, "H.264 NVENC");
        Check(args.find(L"-vf") == std::wstring::npos, "no CPU conversion for NV12 into NVENC");
        Check(args.find(L"-colorspace bt709") != std::wstring::npos && args.find(L"-color_range tv") != std::wstring::npos, "tagged BT.709 limited");
        Check(args.find(L"\"D:\\videos\\x.video.mp4\"") != std::wstring::npos, "quoted output");

        job.preset = VideoPreset::HevcNvenc;
        job.layout = render_export::LayoutFor(job.preset, false);
        args = render_export::BuildFfmpegArgs(job);
        Check(args.find(L"-pix_fmt p010le") != std::wstring::npos && args.find(L"main10") != std::wstring::npos, "HEVC main10 from P010");
        job.preset = VideoPreset::Av1Nvenc;
        job.layout = render_export::LayoutFor(job.preset, false);
        args = render_export::BuildFfmpegArgs(job);
        Check(args.find(L"-c:v av1_nvenc") != std::wstring::npos, "AV1 NVENC");

        job.preset = VideoPreset::LosslessFfv1;
        job.layout = render_export::LayoutFor(job.preset, false);
        job.output = L"D:\\videos\\x.video.mkv";
        args = render_export::BuildFfmpegArgs(job);
        Check(args.find(L"-pix_fmt rgba") != std::wstring::npos && args.find(L"-c:v ffv1") != std::wstring::npos, "FFV1 master from RGBA");
        job.preset = VideoPreset::ProRes422Hq;
        job.layout = render_export::LayoutFor(job.preset, false);
        args = render_export::BuildFfmpegArgs(job);
        Check(args.find(L"x2bgr10le") != std::wstring::npos && args.find(L"prores_ks") != std::wstring::npos &&
                  args.find(L"out_color_matrix=bt709") != std::wstring::npos,
            "ProRes from 10 bit RGB, converted with the BT.709 matrix");
        job.preset = VideoPreset::DnxhrHqx;
        job.layout = render_export::LayoutFor(job.preset, false);
        args = render_export::BuildFfmpegArgs(job);
        Check(args.find(L"dnxhr_hqx") != std::wstring::npos, "DNxHR HQX master");
        job.preset = VideoPreset::PreviewH264;
        job.layout = render_export::LayoutFor(job.preset, false);
        args = render_export::BuildFfmpegArgs(job);
        Check(args.find(L"scale=trunc(iw/4)*2") != std::wstring::npos, "preview at half size");

        job.preset = VideoPreset::H264Nvenc;
        job.layout = render_export::OutputLayout::Nv12;
        job.lut = L"C:\\luts\\film.cube";
        args = render_export::BuildFfmpegArgs(job);
        Check(args.find(L"lut3d=file='C\\:/luts/film.cube'") != std::wstring::npos, "LUT path escaped for the filter");
        Check(args.find(L"in_color_matrix=bt709") != std::wstring::npos, "NV12 back to RGB for the LUT");
        job.lut.clear();
        Check(!render_export::InfoOf(VideoPreset::Av1Svt).available, "SVT-AV1 marked missing (essentials build)");

        // Finalize: audio and chapters, video copied.
        job.finalOutput = L"C:\\v\\clip.mp4";
        job.audioWav = L"C:\\v\\a.wav";
        job.chapters = L"C:\\v\\c.txt";
        args = render_export::BuildFinalizeArgs(job, L"C:\\v\\clip.video.mp4", 0.25);
        Check(args.find(L"-c:v copy") != std::wstring::npos, "video copied bit for bit");
        Check(args.find(L"-ss 0.250000 -i \"C:\\v\\a.wav\"") != std::wstring::npos, "audio aligned to frame 0");
        Check(args.find(L"-map 1:a:0") != std::wstring::npos && args.find(L"-map_chapters 2") != std::wstring::npos, "audio and chapters mapped");
        Check(args.find(L"-c:a aac") != std::wstring::npos && args.find(L"+faststart") != std::wstring::npos, "MP4 audio and fast start");
        job.audioWav.clear();
        args = render_export::BuildFinalizeArgs(job, L"C:\\v\\clip.video.mp4", 0.0);
        Check(args.find(L"-map_chapters 1") != std::wstring::npos && args.find(L"-c:a") == std::wstring::npos, "chapters only");

        const std::string meta = render_export::BuildChapterMetadata({{0, "Start"}, {4000, "Shot 2"}, {9000, "a=b"}}, 12000);
        Check(meta.rfind(";FFMETADATA1", 0) == 0, "metadata header");
        Check(meta.find("START=4000\nEND=9000\ntitle=Shot 2") != std::string::npos, "a chapter runs to the next");
        Check(meta.find("START=9000\nEND=12000\ntitle=a\\=b") != std::string::npos, "the last to the end, escaped");

        const std::uint64_t bytes = render_export::EstimateBytes(VideoPreset::H264Nvenc, 3840, 2160, {60, 1}, 60.0);
        Check(bytes > 500000000ull && bytes < 2000000000ull, "4K60 H.264 minute estimate in the hundreds of MB");
        const std::wstring path = render_export::DefaultOutputPath(L"C:\\Videos", L"Clip: 1", VideoPreset::HevcNvenc, 2026, 10, 4, 3, 5, 9);
        Check(path == L"C:\\Videos\\Clip_ 1_20261004-030509_hevc.mp4", "default name");
        Check(render_export::BuildThumbnailArgs(L"v.mp4", 5.0, L"t.jpg").find(L"-ss 5.000") != std::wstring::npos, "thumbnail seek");
    }

    // The in process encoder (libav): variants per preset, the choice from
    // the probe, the options and the libavfilter chain.
    void TestEncoderSelect()
    {
        using replay::exportchannel::VideoPreset;
        using render_export::EncoderBackend;
        using render_export::OutputLayout;
        const auto& all = render_export::AllVariants();
        const VideoPreset dialog[] = {VideoPreset::H264Nvenc, VideoPreset::HevcNvenc, VideoPreset::Av1Nvenc, VideoPreset::PreviewH264,
            VideoPreset::ProRes422Hq, VideoPreset::DnxhrHqx, VideoPreset::LosslessFfv1, VideoPreset::LosslessNvencHevc, VideoPreset::PngSequence,
            VideoPreset::Av1Svt};
        for (VideoPreset preset : dialog)
        {
            const auto variants = render_export::VariantsOf(preset);
            bool cpu = false;
            bool sameLayout = !variants.empty();
            for (std::size_t index : variants)
            {
                cpu = cpu || all[index].backend == EncoderBackend::Cpu;
                // The GPU resolve is set up before the encoder opens: every
                // variant of a preset must take what LayoutFor gives.
                sameLayout = sameLayout && all[index].input == render_export::LayoutFor(preset, false);
            }
            Check(cpu, "every preset has a CPU variant (no GPU encoder: still exports)");
            Check(sameLayout, "the variants of a preset share the GPU layout");
        }
        Check(all[render_export::VariantsOf(VideoPreset::H264Nvenc)[0]].backend == EncoderBackend::Nvenc, "NVENC first");

        render_export::ProbeResult probe;
        probe.done = true;
        probe.opened.assign(all.size(), false);
        Check(render_export::ChooseVariant(VideoPreset::H264Nvenc, probe, true, EncoderBackend::Nvenc) == -1, "nothing opened: no variant");
        auto indexOf = [&all](VideoPreset preset, const char* codec) {
            for (std::size_t i = 0; i < all.size(); ++i)
                if (all[i].preset == preset && std::string(all[i].codec) == codec)
                    return static_cast<int>(i);
            return -1;
        };
        const int x264 = indexOf(VideoPreset::H264Nvenc, "libx264");
        const int nvenc = indexOf(VideoPreset::H264Nvenc, "h264_nvenc");
        const int amf = indexOf(VideoPreset::H264Nvenc, "h264_amf");
        const int prores = indexOf(VideoPreset::ProRes422Hq, "prores_ks");
        probe.opened[static_cast<std::size_t>(x264)] = true;
        Check(render_export::ChooseVariant(VideoPreset::H264Nvenc, probe, true, EncoderBackend::Nvenc) == x264, "CPU fallback (x264)");
        probe.opened[static_cast<std::size_t>(nvenc)] = true;
        probe.opened[static_cast<std::size_t>(amf)] = true;
        probe.opened[static_cast<std::size_t>(prores)] = true;
        Check(render_export::ChooseVariant(VideoPreset::H264Nvenc, probe, true, EncoderBackend::Nvenc) == nvenc, "auto: the GPU first");
        Check(render_export::ChooseVariant(VideoPreset::H264Nvenc, probe, false, EncoderBackend::Amf) == amf, "Encoder=amf forced");
        Check(render_export::ChooseVariant(VideoPreset::H264Nvenc, probe, false, EncoderBackend::Cpu) == x264, "Encoder=cpu forced");
        Check(render_export::ChooseVariant(VideoPreset::H264Nvenc, probe, false, EncoderBackend::Qsv) == -1, "a forced backend that did not open: none");
        Check(render_export::ChooseVariant(VideoPreset::ProRes422Hq, probe, false, EncoderBackend::Nvenc) == prores, "the masters keep the CPU");
        Check(render_export::ChooseVariant(VideoPreset::HevcNvenc, probe, true, EncoderBackend::Nvenc) == -1, "per preset");

        bool automatic = false;
        EncoderBackend backend = EncoderBackend::Cpu;
        Check(render_export::ParseBackendChoice(L"", automatic, backend) && automatic, "Encoder= empty: auto");
        Check(render_export::ParseBackendChoice(L"QSV", automatic, backend) && !automatic && backend == EncoderBackend::Qsv, "Encoder=QSV");
        Check(render_export::ParseBackendChoice(L"cpu", automatic, backend) && backend == EncoderBackend::Cpu, "Encoder=cpu");
        Check(!render_export::ParseBackendChoice(L"bogus", automatic, backend) && automatic, "unknown: auto");

        // Options: the NVENC ones of the ffmpeg.exe path, {q} filled.
        const auto& h264 = all[static_cast<std::size_t>(nvenc)];
        auto options = render_export::VariantOptions(h264, 0);
        auto value = [&options](const char* key) {
            for (const auto& pair : options)
                if (pair.first == key)
                    return pair.second;
            return std::string("<none>");
        };
        Check(value("preset") == "p6" && value("tune") == "hq" && value("rc") == "vbr" && value("b") == "0", "h264_nvenc as measured (p6, hq, vbr)");
        Check(value("cq") == "19", "default CQ 19");
        options = render_export::VariantOptions(h264, 23);
        Check(value("cq") == "23", "the job's quality");
        options = render_export::VariantOptions(all[static_cast<std::size_t>(indexOf(VideoPreset::HevcNvenc, "hevc_nvenc"))], 0);
        Check(value("profile") == "main10" && value("cq") == "20", "HEVC main10 CQ 20");
        options = render_export::VariantOptions(all[static_cast<std::size_t>(indexOf(VideoPreset::LosslessFfv1, "ffv1"))], 0);
        Check(value("level") == "3" && value("slicecrc") == "1" && value("g") == "1", "FFV1 level 3, intra, CRC");

        // Filter chains: nothing on the CPU for NV12/P010 into the GPU encoders.
        using render_export::VideoFilterChain;
        Check(VideoFilterChain(VideoPreset::H264Nvenc, OutputLayout::Nv12, "nv12", false, "").empty(), "NV12 into NVENC: no filter");
        Check(VideoFilterChain(VideoPreset::HevcNvenc, OutputLayout::P010, "p010le", false, "").empty(), "P010 into NVENC: no filter");
        Check(VideoFilterChain(VideoPreset::HevcNvenc, OutputLayout::P010, "yuv420p10le", false, "") == "format=yuv420p10le", "x265: repack only");
        Check(VideoFilterChain(VideoPreset::ProRes422Hq, OutputLayout::X2Bgr10, "yuv422p10le", false, "") ==
                  "scale=out_color_matrix=bt709:out_range=tv,format=yuv422p10le",
            "ProRes: RGB to BT.709 4:2:2 10 bit");
        Check(VideoFilterChain(VideoPreset::LosslessFfv1, OutputLayout::Rgba8, "bgr0", false, "") == "format=bgr0", "FFV1 from RGBA");
        Check(VideoFilterChain(VideoPreset::PreviewH264, OutputLayout::Nv12, "nv12", false, "") ==
                  "scale=trunc(iw/4)*2:trunc(ih/4)*2:flags=bicubic,format=nv12",
            "preview at half size");
        Check(VideoFilterChain(VideoPreset::H264Nvenc, OutputLayout::Nv12, "nv12", false, "C:\\luts\\film.cube") ==
                  "scale=in_color_matrix=bt709:in_range=tv,lut3d=file='C\\:/luts/film.cube',scale=out_color_matrix=bt709:out_range=tv,format=nv12",
            "LUT: to RGB, lut3d, back to BT.709");
        Check(VideoFilterChain(VideoPreset::HevcNvenc, OutputLayout::P010, "p010le", true, "x.cube").find("bt2020") != std::string::npos, "HDR10 LUT in BT.2020");

        Check(std::string(render_export::MuxerFor(L"C:\\v\\a.video.mp4", VideoPreset::H264Nvenc)) == "mp4", "MP4 muxer");
        Check(std::string(render_export::MuxerFor(L"C:\\v\\a.video.mov", VideoPreset::ProRes422Hq)) == "mov", "MOV muxer");
        Check(std::string(render_export::MuxerFor(L"C:\\v\\a.video.mkv", VideoPreset::LosslessFfv1)) == "matroska", "MKV muxer");
        Check(std::string(render_export::MuxerFor(L"C:\\v\\png", VideoPreset::PngSequence)) == "image2", "PNG sequence");
        Check(std::string(render_export::FinalAudioCodec(L"a.mp4")) == "aac" && std::string(render_export::FinalAudioCodec(L"a.mov")) == "pcm_s24le" &&
                  std::string(render_export::FinalAudioCodec(L"a.mkv")) == "flac",
            "audio codec per container (as the ffmpeg.exe finalize)");
        Check(render_export::VariantPixFmt(h264, true) == "p010le", "HDR10 moves NV12 to P010");
    }

    // REPLAYPOC_FFMPEG_ARGS_OUT=<file>: every preset's command line (and the
    // finalize and thumbnail lines) written out, for a real ffmpeg run
    // (tools/render_bench/check_ffmpeg_args.py feeds them raw frames).
    void DumpFfmpegArgs()
    {
        char* path = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&path, &length, "REPLAYPOC_FFMPEG_ARGS_OUT") != 0 || !path)
            return;
        std::FILE* file = nullptr;
        if (fopen_s(&file, path, "wb") == 0 && file)
        {
            using replay::exportchannel::VideoPreset;
            const VideoPreset presets[] = {VideoPreset::H264Nvenc, VideoPreset::HevcNvenc, VideoPreset::Av1Nvenc, VideoPreset::PreviewH264,
                VideoPreset::ProRes422Hq, VideoPreset::DnxhrHqx, VideoPreset::LosslessFfv1, VideoPreset::LosslessNvencHevc, VideoPreset::PngSequence};
            for (VideoPreset preset : presets)
            {
                render_export::EncodeJob job;
                job.width = 1920;
                job.height = 1080;
                job.rate = {60, 1};
                job.preset = preset;
                job.layout = render_export::LayoutFor(preset, false);
                job.output = std::wstring(L"OUTDIR\\") + std::to_wstring(static_cast<unsigned>(preset)) + L".video" + render_export::InfoOf(preset).extension;
                if (preset == VideoPreset::PngSequence)
                    job.output = L"OUTDIR\\png";
                std::fprintf(file, "VIDEO %u %ls\n", static_cast<unsigned>(job.layout), render_export::BuildFfmpegArgs(job).c_str());
                if (preset == VideoPreset::H264Nvenc)
                {
                    job.lut = L"OUTDIR\\identity.cube";
                    std::fprintf(file, "VIDEO %u %ls\n", static_cast<unsigned>(job.layout), render_export::BuildFfmpegArgs(job).c_str());
                    job.lut.clear();
                    job.finalOutput = L"OUTDIR\\final.mp4";
                    job.audioWav = L"OUTDIR\\audio.wav";
                    job.chapters = L"OUTDIR\\chapters.txt";
                    std::fprintf(file, "FINAL %ls\n", render_export::BuildFinalizeArgs(job, job.output, 0.5).c_str());
                    std::fprintf(file, "THUMB %ls\n", render_export::BuildThumbnailArgs(job.finalOutput, 0.5, L"OUTDIR\\thumb.jpg").c_str());
                    std::fprintf(file, "CHAPTERS %s\n", "x");
                }
            }
            std::fclose(file);
        }
        std::free(path);
    }

    void TestStepPlanner()
    {
        // 10 MHz counter, 60 fps: 166666.67 ticks per frame. The engine's
        // gap between our counter read and its own adds 3 ticks per frame;
        // the planner takes them back the next frame.
        replay::exporttimer::StepPlanner planner;
        planner.Begin(1e7);
        std::int64_t total = 0;
        for (int i = 0; i < 36000; ++i)
        {
            const std::int64_t step = FrameStartNs({60, 1}, i + 1) - FrameStartNs({60, 1}, i);
            const std::int64_t ticks = planner.Issue(step);
            const std::int64_t applied = ticks + 3;
            planner.Applied(applied);
            total += applied;
        }
        Check(std::llabs(planner.ErrorTicks()) <= 3, "cumulative error stays within the one frame gap");
        Check(std::llabs(total - 6000000000ll) <= 3, "10 minutes of 60 fps = 6e9 ticks (no drift)");
        // Motion blur: 16 samples at 180 degrees plus the closed step.
        replay::exporttimer::StepPlanner blur;
        blur.Begin(1e7);
        Clock clock;
        Settings settings;
        settings.rate = {60, 1};
        settings.shutter = {16, 180.0f};
        settings.prerollFrames = 0;
        settings.maxFrames = 600;
        clock.Begin(settings);
        std::int64_t wantNs = 0;
        for (Tick t = clock.Next(); t.phase == Phase::Capture; t = clock.Next())
        {
            wantNs += t.stepNs;
            blur.Applied(blur.Issue(t.stepNs));
        }
        Check(std::llabs(blur.ErrorTicks()) <= 1, "blur steps summed exactly");
        Check(blur.Issue(0) >= 1, "never a zero delta");
    }

    void TestTagCells()
    {
        namespace ec = replay::exportchannel;
        for (std::uint64_t serial : {0ull, 1ull, 77ull, 255ull, 256ull + 13ull})
        {
            bool white[ec::kTagCells];
            for (std::uint32_t cell = 0; cell < ec::kTagCells; ++cell)
                white[cell] = ec::TagCellWhite(serial, cell);
            std::uint32_t tag = 0;
            Check(ec::DecodeTag(white, tag) && tag == (serial & 0xFF), "tag round trip");
        }
        bool none[ec::kTagCells] = {};
        std::uint32_t tag = 0;
        Check(!ec::DecodeTag(none, tag), "a picture without the guards is not a tag");
        Check(ec::TagCellCentreX(0) == 0.01f && ec::TagCellCentreY() == 0.015f, "cell geometry");
    }

    void TestConvertReference()
    {
        using namespace render_export;
        // 8 x 2: white, black, red, mid grey; each colour a 2 x 2 block.
        RefPicture picture;
        picture.width = 8;
        picture.height = 2;
        picture.rgba.resize(8 * 2 * 4);
        const float colours[4][3] = {{1, 1, 1}, {0, 0, 0}, {1, 0, 0}, {0.5f, 0.5f, 0.5f}};
        for (std::uint32_t y = 0; y < 2; ++y)
            for (std::uint32_t x = 0; x < 8; ++x)
                for (int k = 0; k < 3; ++k)
                    picture.rgba[(y * 8 + x) * 4 + k] = colours[x / 2][k];
        ExportConstants pc;
        pc.width = 8;
        pc.height = 2;
        pc.format = static_cast<std::uint32_t>(OutputLayout::Nv12);
        std::vector<float> sum;
        std::vector<std::uint8_t> out;
        RefResolve(pc, picture, sum, out);
        Check(out.size() == 8 * 2 * 3 / 2, "NV12 size");
        Check(out[0] == 235 && out[2] == 16, "white Y 235, black Y 16 (limited range)");
        Check(out[16] == 128 && out[17] == 128, "white CbCr neutral");
        Check(out[20] == 102 && out[21] == 240, "red CbCr (BT.709: 102, 240)");
        Check(out[4] == 63, "red Y 63 (BT.709)");
        pc.format = static_cast<std::uint32_t>(OutputLayout::P010);
        RefResolve(pc, picture, sum, out);
        Check(out.size() == 8 * 2 * 3, "P010 size");
        Check((out[0] | (out[1] << 8)) == (940 << 6), "P010 white Y 940 in the high bits");
        pc.format = static_cast<std::uint32_t>(OutputLayout::Rgba8);
        RefResolve(pc, picture, sum, out);
        Check(out[8] == 0 && out[0] == 255 && out[3] == 255 && out[16] == 255 && out[17] == 0, "RGBA packing");
        // Motion blur: two samples, white and black, average in linear light
        // then sRGB: 0.5 linear -> 188 (a gamma average would give 128).
        RefPicture black = picture;
        for (float& v : black.rgba)
            v = 0.0f;
        RefPicture white = picture;
        for (float& v : white.rgba)
            v = 1.0f;
        pc.format = static_cast<std::uint32_t>(OutputLayout::Rgba8);
        pc.flags = kFlagFirst;
        pc.weight = 0.5f;
        RefAccumulate(pc, white, sum);
        pc.flags = 0;
        RefAccumulate(pc, black, sum);
        pc.flags = kFlagFromSum;
        RefResolve(pc, white, sum, out);
        Check(out[0] == 188, "linear light average (sRGB 188)");
        // Letterbox 2.39 on 1920 x 1080: 2 bars of 138 rows.
        std::uint32_t top = 0, bottom = 0;
        LetterboxRows(1920, 1080, 2.39f, top, bottom);
        Check(top == 138 && bottom == 138, "letterbox rows");
        LetterboxRows(1920, 800, 2.39f, top, bottom);
        Check(top == 0 && bottom == 0, "no bars when already wide");
        Check(SizeSupported(3840, 2160) && SizeSupported(2560, 1440) && !SizeSupported(1366, 768), "sizes (width a multiple of 4)");
        Check(!SizeSupported(1366, 767), "odd height refused");
        Check(OutputBytes(OutputLayout::Nv12, 3840, 2160) == 12441600ull, "4K NV12 is 12.4 MB a frame");
        // The output image (R32_UINT, kOutputRowWords wide) holds the frame's
        // bytes in order; the readback copies whole rows.
        Check(OutputImageRows(OutputLayout::Nv12, 3840, 2160) == 760u, "4K NV12 output image is 760 rows of 4096 words");
        Check(OutputImageBytes(OutputLayout::Nv12, 3840, 2160) >= OutputBytes(OutputLayout::Nv12, 3840, 2160), "output image holds the frame");
        Check(OutputImageRows(OutputLayout::X2Bgr10, 3840, 2160) == 2025u && OutputImageRows(OutputLayout::Nv12, 64, 32) == 1u,
            "output image rows (4K RGB, tiny)");
    }

    // The blur's samples per frame never put a world step under the engine's
    // minimum (below it the export crashed).
    void TestSlowMotionBlur()
    {
        const FrameRate r60{60, 1};
        const std::int64_t clampNs = 333333; // [0x3CC8388] = 1/3000 s, logged clamp_min=0.000333
        const Shutter crash{32, 180.0f};     // the export that crashed
        Check(SamplesForWorldScale(r60, crash, 0, 1.0, clampNs) == 25, "32 samples at 60 fps, 180 deg: 25 at 100 % (260 us < 333 us)");
        Check(SamplesForWorldScale(r60, crash, 0, 0.2, clampNs) == 5, "20 % slow motion: 5 samples (333 us of world each)");
        Check(SamplesForWorldScale(r60, crash, 0, 0.05, clampNs) == 1, "5 %: one sample (no blur)");
        Check(SamplesForWorldScale(r60, {8, 180.0f}, 0, 1.0, clampNs) == 8, "8 samples fit at 100 %");
        Check(SamplesForWorldScale(r60, {8, 180.0f}, 0, 0.35, clampNs) == 8, "8 samples fit at 35 %");
        Check(SamplesForWorldScale(r60, {16, 360.0f}, 0, 0.2, clampNs) == 10, "360 deg at 20 %: 10");
        Check(SamplesForWorldScale(r60, {1, 180.0f}, 0, 0.05, clampNs) == 1, "no blur stays one sample");
        Check(SamplesForWorldScale(r60, crash, 0, 0.2, 0) == 32, "no clamp known: as asked");
        Check(!FrameStepBelowMinimum(r60, 0, 0.05, clampNs), "5 % at 60 fps: the frame step (833 us) is above the minimum");
        Check(FrameStepBelowMinimum(r60, 0, 0.0025, clampNs), "5 % x 5 %: below even without blur");
        // Every sample's world step at or above the minimum for any slow down.
        for (double scale : {1.0, 0.75, 0.5, 0.35, 0.2, 0.1, 0.05})
        {
            const std::uint32_t n = SamplesForWorldScale(r60, crash, 0, scale, clampNs);
            const double spacing = static_cast<double>(OpenNs(r60, {n, 180.0f}, 0)) / n;
            Check(n == 1 || spacing * scale >= clampNs * 0.999, "the sample spacing times the scale reaches the minimum");
            Check(n == 32 || n == 1 || (static_cast<double>(OpenNs(r60, {n + 1, 180.0f}, 0)) / (n + 1)) * scale < clampNs, "and one more sample would not");
        }
    }

    // The clock takes the cap at a frame's first sample only: a frame never
    // changes its count midway, its weights add up to one, and an export at
    // 100 % then 20 % from frame 10 never steps the world under the minimum
    // between samples.
    void TestClockSampleCap()
    {
        const FrameRate r60{60, 1};
        const std::int64_t clampNs = 333333;
        Settings settings;
        settings.rate = r60;
        settings.shutter = {32, 180.0f};
        settings.prerollFrames = 2;
        settings.maxFrames = 20;
        Clock clock;
        clock.Begin(settings);
        auto scaleOf = [](std::int64_t frame) { return frame < 10 ? 1.0 : 0.2; };
        double weight = 0.0;
        std::int64_t frame = -100;
        std::uint32_t frameSamples = 0;
        std::uint32_t seen = 0;
        bool belowMinimum = false;
        bool countChangedMidFrame = false;
        bool weightsOk = true;
        std::uint32_t samplesAt[20] = {};
        double previousScale = 1.0;
        for (int guard = 0; guard < 10000; ++guard)
        {
            const Tick tick = clock.Next();
            if (tick.phase == Phase::Done)
                break;
            // The step of this tick runs at the scale the world had then.
            if (tick.phase == Phase::Capture && !tick.firstOfFrame && tick.stepNs * previousScale < clampNs * 0.999)
                belowMinimum = true;
            if (tick.phase == Phase::Capture)
            {
                if (tick.firstOfFrame)
                {
                    frame = tick.frame;
                    frameSamples = tick.samples;
                    weight = 0.0;
                    seen = 0;
                }
                else if (tick.samples != frameSamples || tick.frame != frame)
                    countChangedMidFrame = true;
                weight += tick.weight;
                ++seen;
                if (tick.lastOfFrame)
                {
                    weightsOk = weightsOk && std::fabs(weight - 1.0) < 1e-5 && seen == frameSamples;
                    if (tick.frame >= 0 && tick.frame < 20)
                        samplesAt[tick.frame] = frameSamples;
                }
                previousScale = scaleOf(tick.frame);
            }
            // EndTick: the cap for a frame starting next, from the scale ahead
            // (the next frame's), and a cap change mid frame must be ignored.
            const std::int64_t next = tick.phase == Phase::Capture ? (tick.lastOfFrame ? tick.frame + 1 : tick.frame) : 0;
            clock.SetSampleCap(SamplesForWorldScale(r60, settings.shutter, next, std::min(scaleOf(next), scaleOf(next + 1)), clampNs));
            if (tick.phase == Phase::Capture && tick.sample == 1)
                clock.SetSampleCap(32); // ignored until the next frame starts... and replaced by the EndTick above on the last sample
        }
        Check(!countChangedMidFrame, "a frame keeps the count it started with");
        Check(weightsOk, "each frame's weights add up to one over its own samples");
        Check(samplesAt[0] == 25 && samplesAt[8] == 25, "25 samples at 100 %");
        Check(samplesAt[9] == 5, "the frame before the slow motion already takes 5 (the look ahead)");
        Check(samplesAt[10] == 5 && samplesAt[19] == 5, "5 samples in the 20 % slow motion");
        Check(!belowMinimum, "no world step between samples under the engine's minimum");
    }

    // timer_check: the clamp counts, and a scale change may land a frame late.
    void TestTimerCheck()
    {
        const double clamp = 1.0 / 3000.0;
        Check(CheckTimerStep(0.000260417, 1.0, 1.0, 0.000333, clamp) == TimerCheck::Clamped,
            "100 % with 32 samples: the engine ran its minimum (was hidden by the 0.2 ms tolerance)");
        Check(CheckTimerStep(0.000260417, 0.2, 0.2, 0.000333, clamp) == TimerCheck::Clamped, "20 % with 32 samples: clamped (the crash export)");
        Check(CheckTimerStep(0.0016667, 0.2, 0.2, 0.000333, clamp) == TimerCheck::Match, "20 % with 5 samples: the planned step");
        Check(CheckTimerStep(0.0016667, 0.2, 1.0, 0.0016667, clamp) == TimerCheck::Match, "the frame the scale changed may still run the old one");
        Check(CheckTimerStep(0.0016667, 0.2, 1.0, 0.000333, clamp) == TimerCheck::Match, "or already the new one");
        Check(CheckTimerStep(0.0016667, 0.2, 0.2, 0.0016667, clamp) == TimerCheck::Mismatch, "a steady 20 % running at 100 % is a mismatch");
        Check(CheckTimerStep(0.0166667, 1.0, 1.0, 0.0167, clamp) == TimerCheck::Match, "a whole frame at 100 %");
        Check(CheckTimerStep(0.0126302, 1.0, 1.0, 0.0126, clamp) == TimerCheck::Match, "the closed shutter step");
    }

    // The dialog's plan from the markers (video seconds of each span).
    void TestBlurPlan()
    {
        const std::vector<MarkerSpeed> markers = {{0, 1.0}, {5000, 0.2}, {8000, 1.0}};
        const std::vector<SpeedSpan> spans = SpeedSpansFromMarkers(markers, 2000, 10000, 1.0);
        Check(spans.size() == 3 && spans[0].startMs == 2000 && spans[0].endMs == 5000 && spans[1].speed == 0.2 && spans[2].endMs == 10000,
            "speed spans from the markers, cut to the range");
        Check(SpeedSpansFromMarkers(markers, 6000, 7000, 0.5).size() == 1 && SpeedSpansFromMarkers(markers, 6000, 7000, 0.5)[0].speed == 0.1,
            "a range inside one marker, times the playback speed");
        const std::vector<BlurSpan> plan = PlanBlurSpans({60, 1}, {32, 180.0f}, spans, kEngineMinStepS);
        Check(plan.size() == 3, "100 %, 20 % and 100 % again are three spans");
        bool slowFound = false;
        for (const BlurSpan& span : plan)
            if (span.worldScale < 1.0)
            {
                slowFound = true;
                Check(span.samples == 5, "the slow motion takes 5 samples");
                Check(std::fabs(span.outStartS - 3.0) < 1e-9 && std::fabs(span.outEndS - 18.0) < 1e-9,
                    "3 s of playhead at 20 % is 15 s of video, from 3 s to 18 s");
            }
        Check(slowFound, "the slow motion span is listed");
        Check(PlanBlurSpans({60, 1}, {8, 180.0f}, SpeedSpansFromMarkers({}, 0, 4000, 1.0), kEngineMinStepS).empty(),
            "8 samples at 100 %: nothing to say");
        const std::vector<BlurSpan> slow8 = PlanBlurSpans({60, 1}, {8, 180.0f}, spans, kEngineMinStepS);
        Check(slow8.size() == 1 && slow8[0].samples == 5, "8 samples: only the slow motion is reduced");
    }

    // The video pass's muxer options (a file cut by a crash opens).
    void TestCrashSafeMux()
    {
        using replay::exportchannel::VideoPreset;
        auto has = [](const std::vector<std::pair<std::string, std::string>>& options, const char* key, const char* part) {
            for (const auto& option : options)
                if (option.first == key && option.second.find(part) != std::string::npos)
                    return true;
            return false;
        };
        const auto h264 = render_export::CrashSafeMuxOptions("mp4", VideoPreset::H264Nvenc);
        Check(has(h264, "movflags", "frag_keyframe") && has(h264, "movflags", "empty_moov") && has(h264, "movflags", "default_base_moof"),
            "MP4 long GOP: fragmented at keyframes, empty moov first");
        Check(has(h264, "frag_size", "67108864") && has(h264, "frag_duration", "1000000") && !has(h264, "movflags", "write_colr"),
            "64 MB and one second per fragment at most; no colr flag in MP4");
        const auto prores = render_export::CrashSafeMuxOptions("mov", VideoPreset::ProRes422Hq);
        Check(has(prores, "movflags", "write_colr") && has(prores, "frag_duration", "1000000") && !has(prores, "movflags", "frag_keyframe"),
            "ProRes MOV: one second fragments (every frame is a keyframe), colr kept");
        const auto ffv1 = render_export::CrashSafeMuxOptions("matroska", VideoPreset::LosslessFfv1);
        Check(has(ffv1, "cluster_time_limit", "1000") && !has(ffv1, "movflags", ""), "Matroska: one second clusters");
        Check(render_export::CrashSafeMuxOptions("image2", VideoPreset::PngSequence).empty(), "PNG sequence: nothing");
        Check(render_export::FragmentedMuxer("mp4") && render_export::FragmentedMuxer("mov") && !render_export::FragmentedMuxer("matroska"),
            "the finalize always remuxes MP4 and MOV");
        render_export::EncodeJob job;
        job.width = 1920;
        job.height = 1080;
        job.preset = VideoPreset::H264Nvenc;
        job.layout = render_export::LayoutFor(job.preset, false);
        job.output = L"D:\\videos\\x.video.mp4";
        std::wstring args = render_export::BuildFfmpegArgs(job);
        Check(args.find(L"-movflags +frag_keyframe+empty_moov+delay_moov+default_base_moof -frag_duration 1000000 -frag_size 67108864 \"D:\\videos\\x.video.mp4\"") != std::wstring::npos,
            "ffmpeg.exe backend: the same flags before the output");
        job.preset = VideoPreset::ProRes422Hq;
        job.layout = render_export::LayoutFor(job.preset, false);
        job.output = L"D:\\videos\\x.video.mov";
        args = render_export::BuildFfmpegArgs(job);
        Check(args.find(L"+write_colr") != std::wstring::npos && args.find(L"-frag_duration 1000000") != std::wstring::npos &&
                  args.find(L"-movflags +write_colr") == std::wstring::npos,
            "ffmpeg.exe ProRes: one movflags with write_colr");
    }
}

namespace
{
    // The get_query_heap_results ABI by ReShade version.
    // The thumbnail and QA shot readback buffers (editor/readback_release.h):
    // freed only when their worker is done, leaked while it still reads the
    // mapping; the wait on the render thread is bounded by time.
    void TestReadbackRelease()
    {
        using editor_readback::Dispose;
        const editor_readback::Disposal none = Dispose(false, false, false);
        Check(!none.unmap && !none.destroy && !none.leak, "no buffer: nothing to do");
        const editor_readback::Disposal idleMapped = Dispose(true, true, false);
        Check(idleMapped.unmap && idleMapped.destroy && !idleMapped.leak, "worker done, still mapped: unmapped then destroyed");
        const editor_readback::Disposal idle = Dispose(true, false, false);
        Check(!idle.unmap && idle.destroy && !idle.leak, "worker done, not mapped: destroyed");
        const editor_readback::Disposal busy = Dispose(true, true, true);
        Check(!busy.unmap && !busy.destroy && busy.leak, "worker still reading: neither unmapped nor destroyed (leaked)");

        // A worker that finishes after a few polls.
        std::uint64_t clock = 0;
        int polls = 0;
        bool ok = editor_readback::WaitIdle([&polls] { return ++polls < 4; }, 1000, [&clock] { return clock; }, [&clock] { clock += 16; });
        Check(ok && polls == 4, "a worker done within the limit: idle");
        // A worker that never finishes, with a coarse 16 ms sleep: the limit
        // is time (about 63 pauses), not 1000 pauses of 16 ms.
        clock = 0;
        int pauses = 0;
        ok = editor_readback::WaitIdle([] { return true; }, 1000, [&clock] { return clock; }, [&clock, &pauses] { clock += 16; ++pauses; });
        Check(!ok, "a stuck worker: the wait gives up");
        Check(clock >= 1000 && clock < 1000 + 16 && pauses <= 63, "the wait gives up at its time limit");
    }

    // The audio pass waits for the add-on's capture (export_channel.h
    // AudioSeekStepFor): the playhead never leaves the start before the
    // capture runs, a failed capture skips the pass, silence fails in time.
    void TestAudioSeekAck()
    {
        namespace ec = replay::exportchannel;
        const auto pending = static_cast<std::uint32_t>(ec::AudioAck::Pending);
        const auto capturing = static_cast<std::uint32_t>(ec::AudioAck::Capturing);
        const auto failed = static_cast<std::uint32_t>(ec::AudioAck::Failed);
        Check(ec::AudioSeekStepFor(pending, 0) == ec::AudioSeekStep::Hold, "no answer yet: the playhead stays at the start");
        Check(ec::AudioSeekStepFor(pending, ec::kAudioReadyTimeoutMs - 1) == ec::AudioSeekStep::Hold, "no answer, still in time: held");
        Check(ec::AudioSeekStepFor(pending, ec::kAudioReadyTimeoutMs) == ec::AudioSeekStep::Fail, "no answer in time: the export fails");
        Check(ec::AudioSeekStepFor(capturing, 0) == ec::AudioSeekStep::Play, "the capture runs: the pass plays");
        Check(ec::AudioSeekStepFor(capturing, ec::kAudioReadyTimeoutMs * 2) == ec::AudioSeekStep::Play,
            "a late answer that arrives is still played (the add-on started its capture)");
        Check(ec::AudioSeekStepFor(failed, 0) == ec::AudioSeekStep::SkipAudio, "the capture failed: no pass, the video kept without sound");
        Check(ec::AudioSeekStepFor(7u, 0) == ec::AudioSeekStep::Hold, "an unknown value is no answer");
        Check(ec::kAudioReadyTimeoutMs > 5000, "the timeout covers the capture's own 5 s activation wait");
        ec::Channel channel;
        Check(channel.audioCapture.load() == pending, "a new channel has no answer");
        Check(ec::kVersion >= 3, "the acknowledgement is protocol v3 or later (an older add-on or probe refuses the channel)");
    }

    // The finisher's wait for the audio pass (editor/render_export/export_finish.h):
    // Shutdown's stop ends it whatever the probe does.
    // The output frames at the playback's speeds (SpanOutputFrames): a 50 %
    // slow motion doubles them, 20 % five times, 200 % halves them, and at 1x
    // they are the span's frame count (the end of an export).
    void TestSpanOutputFrames()
    {
        using replay::exporting::FrameRate;
        using replay::exporting::SpanOutputFrames;
        const FrameRate fps60{60, 1};
        const auto at = [](double speed) { return [speed](std::uint64_t) { return speed; }; };
        Check(SpanOutputFrames(1000, 9000, fps60, at(1.0), 1000000) == 480, "8 s at 1x: 480 frames, as the span count");
        Check(SpanOutputFrames(0, 1001, fps60, at(1.0), 1000000) == (1001ull * 60 / 1000), "a span off the frame grid: the same floor");
        Check(SpanOutputFrames(1000, 9000, fps60, at(0.5), 1000000) == 960, "50 %: twice the frames (the whole span, not its first half)");
        Check(SpanOutputFrames(1000, 9000, fps60, at(0.2), 1000000) == 2400, "20 %: five times");
        Check(SpanOutputFrames(1000, 9000, fps60, at(2.0), 1000000) == 240, "200 %: half");
        const auto mixed = [](std::uint64_t ms) { return ms < 5000 ? 1.0 : 0.25; };
        Check(SpanOutputFrames(1000, 9000, fps60, mixed, 1000000) == 240 + 960, "4 s at 1x, then 4 s at 25 %");
        Check(SpanOutputFrames(0, 1000, FrameRate{30000, 1001}, at(1.0), 1000000) == 29, "29.97 fps, 1 s: 29 whole frames");
        Check(SpanOutputFrames(0, 1000, fps60, at(0.0), 1000000) == 60, "a zero rate counts at 1x");
        Check(SpanOutputFrames(0, 100000, fps60, at(0.0001), 50) == 50, "the cap");
        Check(SpanOutputFrames(5000, 5000, fps60, at(1.0), 1000000) == 0, "an empty span");
    }

    void TestAudioWaitStep()
    {
        using render_export::AudioWaitEnd;
        using render_export::AudioWaitStep;
        using replay::exportchannel::State;
        Check(AudioWaitStep(State::AudioSeek, false, false) == AudioWaitEnd::Waiting, "seeking: wait");
        Check(AudioWaitStep(State::AudioPass, false, false) == AudioWaitEnd::Waiting, "playing: wait");
        Check(AudioWaitStep(State::AudioDone, false, false) == AudioWaitEnd::AudioDone, "the pass ended");
        Check(AudioWaitStep(State::Failed, false, false) == AudioWaitEnd::ProbeFailed, "the probe failed the export");
        Check(AudioWaitStep(State::Cancelled, false, false) == AudioWaitEnd::Cancelled, "cancelled: the export fails, no finalize");
        Check(AudioWaitStep(State::Off, false, false) == AudioWaitEnd::ProbeEnded, "the probe reloaded (channel reset)");
        Check(AudioWaitStep(State::AudioPass, true, false) == AudioWaitEnd::Deadline, "the wait's own limit");
        Check(AudioWaitStep(State::AudioPass, false, true) == AudioWaitEnd::Stopped, "Shutdown's stop ends a running pass wait");
        Check(AudioWaitStep(State::AudioSeek, false, true) == AudioWaitEnd::Stopped, "and a wait for a probe that never ticks again");
        Check(AudioWaitStep(State::AudioDone, true, true) == AudioWaitEnd::Stopped, "the stop wins over everything");
    }

    // The finalize's outcome (export_finish.h ReportFinalize): Done only when
    // the requested file exists, a warning when it lost its sound, and the
    // temporary file named when the video could not be saved.
    void TestReportFinalize()
    {
        using render_export::ReportFinalize;
        const std::string finalPath = "C:\\V\\clip.mp4";
        const std::string tempPath = "C:\\V\\clip.video.mp4";
        auto r = ReportFinalize(true, true, false, finalPath, tempPath);
        Check(r.ok && !r.warning && r.detail == finalPath, "remuxed: done, the file");
        r = ReportFinalize(true, false, true, finalPath, tempPath);
        Check(r.ok && r.warning && r.detail.rfind(finalPath, 0) == 0 && r.detail.find("warning") != std::string::npos,
            "remux failed, video moved: done with the warning kept in the text");
        r = ReportFinalize(true, false, false, finalPath, tempPath);
        Check(!r.ok && r.detail.find(tempPath) != std::string::npos && r.detail.find("finalize_failed") != std::string::npos,
            "remux failed and the fallback move failed: a failure naming the surviving temporary file");
        r = ReportFinalize(false, false, true, finalPath, tempPath);
        Check(r.ok && !r.warning && r.detail == finalPath, "no remux needed, renamed: done");
        r = ReportFinalize(false, false, false, finalPath, tempPath);
        Check(!r.ok && r.detail.find(tempPath) != std::string::npos && r.detail.find("rename_failed") != std::string::npos,
            "the rename failed: a failure naming the temporary file");
    }

    void TestReShadeQueryAbi()
    {
        using render_export::QueryAbi;
        using render_export::QueryAbiForVersion;
        Check(QueryAbiForVersion("6.6.2") == QueryAbi::Api18, "ReShade 6.6.2 (the submodule's version) is API 18");
        Check(QueryAbiForVersion("6.6.2.2081") == QueryAbi::Api18, "a four part version reads like three");
        Check(QueryAbiForVersion("6.7.3") == QueryAbi::Api18, "ReShade 6.7.3 is still API 18");
        Check(QueryAbiForVersion("6.7.0 UNOFFICIAL") == QueryAbi::Api18, "a self built copy keeps its version");
        Check(QueryAbiForVersion("6.0.0") == QueryAbi::Api18, "older 6.x would not load the add-on anyway: API 18 path");
        Check(QueryAbiForVersion("6.8.0") == QueryAbi::Api20, "ReShade 6.8.0 (the QAs' 6.8.0.2155) is API 20");
        Check(QueryAbiForVersion("6.8.0.2155") == QueryAbi::Api20, "6.8 four part version");
        Check(QueryAbiForVersion("6.8.4 UNOFFICIAL") == QueryAbi::Api20, "6.8 self built copy");
        Check(QueryAbiForVersion("6.9.0") == QueryAbi::Unknown, "a newer minor is refused, not guessed");
        Check(QueryAbiForVersion("6.80.0") == QueryAbi::Unknown, "6.80 is not 6.8");
        Check(QueryAbiForVersion("7.0.0") == QueryAbi::Unknown, "a newer major is refused");
        Check(QueryAbiForVersion("5.9.2") == QueryAbi::Unknown, "ReShade 5 is refused");
        Check(QueryAbiForVersion("") == QueryAbi::Unknown, "an empty version is refused");
        Check(QueryAbiForVersion(nullptr) == QueryAbi::Unknown, "no version is refused");
        Check(QueryAbiForVersion("6") == QueryAbi::Unknown, "no minor is refused");
        Check(QueryAbiForVersion("6.x") == QueryAbi::Unknown, "a non numeric minor is refused");
        Check(QueryAbiForVersion("6.8beta") == QueryAbi::Unknown, "trailing text right after the minor is refused");
        Check(QueryAbiForVersion("99999999999.8") == QueryAbi::Unknown, "an overflowing major is refused");
        Check(std::string(render_export::QueryAbiName(QueryAbi::Api18)) == "18" &&
                  std::string(render_export::QueryAbiName(QueryAbi::Api20)) == "20" &&
                  std::string(render_export::QueryAbiName(QueryAbi::Unknown)) == "unknown",
            "ABI names for the log");
    }

    // A fake device whose vtable has 33 entries; slot 32 records what it got
    // with the API 20 argument list.
    struct QueryCall
    {
        void* self = nullptr;
        std::uint64_t heap = 0;
        std::uint32_t type = 0, first = 0, count = 0, stride = 0;
        void* results = nullptr;
        int calls = 0;
    };
    QueryCall g_queryCall;

    bool FakeSlotApi20(void* self, std::uint64_t heap, std::uint32_t type, std::uint32_t first, std::uint32_t count, void* results,
        std::uint32_t stride)
    {
        g_queryCall = {self, heap, type, first, count, stride, results, g_queryCall.calls + 1};
        static_cast<std::uint64_t*>(results)[0] = 0x1234;
        return true;
    }

    bool FakeOtherSlot(void*, std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, void*, std::uint32_t)
    {
        g_queryCall.calls += 1000;
        return false;
    }

    void TestQueryResultsApi20Call()
    {
        void* table[render_export::kQueryResultsSlot + 1];
        for (void*& entry : table)
            entry = reinterpret_cast<void*>(&FakeOtherSlot);
        table[render_export::kQueryResultsSlot] = reinterpret_cast<void*>(&FakeSlotApi20);
        struct FakeDevice
        {
            void** vtable;
        } device{table};
        Check(render_export::kQueryResultsSlot * sizeof(void*) == 0x100, "slot 32 is +0x100, the core's call [rax+0x100]");
        std::uint64_t stamps[5] = {};
        g_queryCall = {};
        const bool ok = render_export::CallQueryResultsApi20(&device, 0xC0FFEE00ull, 3u, 10u, 5u, stamps, 8u);
        Check(ok && g_queryCall.calls == 1, "the API 20 call lands on slot 32 only");
        Check(g_queryCall.self == &device, "this is the device");
        Check(g_queryCall.heap == 0xC0FFEE00ull && g_queryCall.type == 3u && g_queryCall.first == 10u && g_queryCall.count == 5u &&
                  g_queryCall.results == stamps && g_queryCall.stride == 8u,
            "every API 20 argument in its place (heap, type, first, count, results, stride)");
        Check(stamps[0] == 0x1234, "the results pointer is the one written");
    }
}

int RunExportScheduleTests()
{
    TestFrameStart();
    TestSampleTimes();
    TestClockCapture();
    TestClockFinish();
    TestClosedShutterStep();
    TestTag();
    TestStampRing();
    TestReadbackRing();
    TestFfmpegArgs();
    TestEncoderSelect();
    DumpFfmpegArgs();
    TestStepPlanner();
    TestTagCells();
    TestConvertReference();
    TestSlowMotionBlur();
    TestClockSampleCap();
    TestTimerCheck();
    TestBlurPlan();
    TestCrashSafeMux();
    TestReShadeQueryAbi();
    TestQueryResultsApi20Call();
    TestReadbackRelease();
    TestAudioSeekAck();
    TestSpanOutputFrames();
    TestAudioWaitStep();
    TestReportFinalize();
    if (g_exportFailures == 0)
        std::printf("export schedule tests passed\n");
    return g_exportFailures;
}
