#pragma once

// Render export: shared memory between ReplayProbe (the script: playhead,
// export clock, game timer) and the editor add-on (ReShade: capture,
// accumulation, readback, encoder). The probe creates it (named per process),
// the add-on opens it and refuses another layout (`bytes`).
//
// Flow:
//   1. The editor's Export dialog writes `settings` and bumps requestSeq,
//      then sends CommandType::StartExport. (settings live here, not in the
//      Command: the command layout does not change.)
//   2. The probe seeks to the start, waits for the settle, sets Arming.
//   3. The add-on creates its textures, starts the encoder, sets addonReady.
//   4. The probe runs the export clock: every tick it writes one TickStamp
//      (ring, by serial) and advances probeSerial; the game timer is fixed
//      to the tick's step.
//   5. The add-on, at the capture point of every present, takes the stamp of
//      serial (presents since begin - latency), accumulates Capture ticks and
//      queues each finished frame to the encoder. When its queue is full it
//      blocks the present (the whole game waits; with a fixed timestep a
//      wait changes nothing in the picture).
//   6. Flush, Finishing (encoder drains, audio muxed), Done or Failed.
//      With the audio pass, AudioSeek holds the playhead at the start until
//      the add-on acknowledges its capture (audioCapture): ready plays the
//      range, failed skips the pass (the video is kept without sound), no
//      answer within kAudioReadyTimeoutMs fails the export.
// Off unless RedemptionEditor.ini [Export] Enabled=1.

#include "export_schedule.h"

#include <atomic>
#include <cstdint>

namespace replay::exportchannel
{
    constexpr std::uint32_t kMagic = 0x31505845; // "EXP1"
    // v2: audio pass, look options, stage timings, tag geometry; v3: audio capture
    // acknowledgement; v4: whole project export (SegmentDone, the segment fields).
    constexpr std::uint32_t kVersion = 4;
    constexpr std::uint32_t kTickRing = 256; // > any render latency
    constexpr std::uint32_t kPathCapacity = 260;

    enum class State : std::uint32_t
    {
        Off = 0,
        Requested = 1, // the editor asked; the probe seeks and settles
        Arming = 2,    // playhead at the start, time held; waits for addonReady
        Preroll = 3,
        Capture = 4,
        Flush = 5,
        Finishing = 6, // the probe is done; the add-on drains the encoder
        Done = 7,
        Failed = 8,
        Cancelled = 9,
        // Audio pass (Settings::audio == RealtimePass), after Finishing: the
        // probe seeks to the start and plays the range at 1x on the wall
        // clock while the add-on captures the game's own audio (WASAPI
        // process loopback); AudioDone when the playhead passed the end.
        AudioSeek = 10,
        AudioPass = 11,
        AudioDone = 12,
        // Whole project export: a piece's video (and audio pass) is done and
        // kept by the add-on; the script opens the next clip and requests the
        // next piece. Not idle: the export goes on (cancel still applies).
        SegmentDone = 13
    };

    enum class CaptureStage : std::uint32_t
    {
        AfterEffects = 0,  // reshade_finish_effects: the game plus the player's ReShade effects, no UI
        BeforeEffects = 1  // reshade_begin_effects: the game's own picture
    };

    enum class AudioMode : std::uint32_t
    {
        None = 0,
        RealtimePass = 1, // a second pass at 1x capturing the process audio (WASAPI process loopback)
        EngineLocked = 2  // the game's mixer stepped with the export clock
    };

    enum class Container : std::uint32_t
    {
        Mkv = 0,
        Mp4 = 1,
        Mov = 2,
        ImageSequence = 3
    };

    // The encoder preset (editor/render_export/ffmpeg_args.h builds the command line).
    enum class VideoPreset : std::uint32_t
    {
        LosslessFfv1 = 0,   // master: FFV1 in MKV, RGB 4:4:4, exact
        LosslessNvencHevc = 1,
        Av1Nvenc = 2,
        HevcNvenc = 3,
        H264Nvenc = 4,
        ProRes422Hq = 5,
        PngSequence = 6,
        Av1Svt = 7,         // software AV1 (libsvtav1): slow, best size (not in the essentials ffmpeg build)
        DnxhrHqx = 8,       // master: DNxHR HQX 10 bit 4:2:2 in MOV (CPU)
        PreviewH264 = 9     // light preview: H.264 NVENC, half size, small file
    };

    // The add-on's answer to AudioSeek (Channel::audioCapture).
    enum class AudioAck : std::uint32_t
    {
        Pending = 0,   // no answer yet (reset by the probe at every request)
        Capturing = 1, // the process loopback capture runs: the pass may play
        Failed = 2     // the capture did not start: no pass, the video is kept without sound
    };

    // How long the probe holds AudioSeek (after the seek settled) for the
    // acknowledgement. The capture's activation waits up to 5 s; the add-on
    // starts it once its ring is drained.
    constexpr std::uint32_t kAudioReadyTimeoutMs = 30000;

    enum class AudioSeekStep : std::uint32_t
    {
        Hold = 0,      // keep the playhead at the start
        Play = 1,      // the capture runs: play the range at 1x
        SkipAudio = 2, // the capture failed: straight to AudioDone without a pass
        Fail = 3       // no answer in time: the export fails
    };

    // The probe's AudioSeek step once the seek settled. Pure.
    inline AudioSeekStep AudioSeekStepFor(std::uint32_t ack, std::uint64_t waitedMs)
    {
        if (ack == static_cast<std::uint32_t>(AudioAck::Capturing))
            return AudioSeekStep::Play;
        if (ack == static_cast<std::uint32_t>(AudioAck::Failed))
            return AudioSeekStep::SkipAudio;
        return waitedMs >= kAudioReadyTimeoutMs ? AudioSeekStep::Fail : AudioSeekStep::Hold;
    }

    struct Settings
    {
        std::uint64_t startMs = 0; // playhead of frame 0
        std::uint64_t endMs = 0;   // the capture ends at the frame where the playhead passes it
        replay::exporting::FrameRate rate;
        std::uint32_t samples = 1;
        float shutterDegrees = 180.0f;
        std::uint32_t prerollFrames = 30;
        std::uint32_t width = 0;  // 0 = the back buffer's
        std::uint32_t height = 0;
        CaptureStage stage = CaptureStage::AfterEffects;
        VideoPreset preset = VideoPreset::LosslessFfv1;
        Container container = Container::Mkv;
        AudioMode audio = AudioMode::RealtimePass;
        std::uint32_t quality = 0;    // preset specific (CQ / CRF); 0 = the preset's default
        std::uint32_t hdrOutput = 0;  // 0 SDR BT.709; 1 HDR10 (PQ, BT.2020) when the swap chain is HDR
        wchar_t outputPath[kPathCapacity] = {};
        // v2: the look (on the GPU in the resolve, except the LUT: ffmpeg lut3d).
        float letterboxAspect = 0.0f; // 0 off, 2.39 scope
        float grain = 0.0f;           // film grain amplitude, code values / 255 (0 off)
        wchar_t lutPath[kPathCapacity] = {}; // .cube; empty = none
        std::uint32_t thumbnail = 1;  // a JPEG next to the video, from the middle frame
        std::uint32_t chapters = 1;   // the markers become MP4/MKV chapters
        std::int32_t audioOffsetMs = 0; // added to the audio pass alignment (output latency)
    };

    // Stage timings published by the add-on (Channel::stageUs).
    enum Stage : std::uint32_t
    {
        kStageGpuCopy = 0,     // back buffer -> scratch (GPU timestamp)
        kStageGpuAccumulate,   // CSAccumulate (GPU)
        kStageGpuResolve,      // CSResolve (GPU)
        kStageGpuReadback,     // output buffer -> staging slot (GPU)
        kStageCpuMap,          // map + copy out of the staging slot (CPU)
        kStageCpuQueueWait,    // wait on the encoder queue (backpressure)
        kStageFrameWall,       // wall time per output frame (CPU)
        kStageRingWait,        // wait for the oldest staging slot when the ring is full
        kStageCount
    };

    // Latency tag: during preroll and flush the probe draws kTagCells rectangles
    // along the top left of the screen: a white guard, the 8 low bits of the tick
    // serial (white = 1), a black guard. Normalised screen units (DRAW_RECT takes
    // centres).
    constexpr std::uint32_t kTagCells = 10;
    constexpr float kTagCellWidth = 0.02f;
    constexpr float kTagCellHeight = 0.03f;
    inline float TagCellCentreX(std::uint32_t cell) { return (static_cast<float>(cell) + 0.5f) * kTagCellWidth; }
    inline float TagCellCentreY() { return 0.5f * kTagCellHeight; }
    // Cell value: 0 guard white, 1..8 serial bit (cell - 1), 9 guard black.
    inline bool TagCellWhite(std::uint64_t serial, std::uint32_t cell)
    {
        if (cell == 0)
            return true;
        if (cell >= kTagCells - 1)
            return false;
        return replay::exporting::TagBit(serial, cell - 1);
    }
    // Decodes the cells read back (true = bright); false when the guards are
    // not there (no tag in that picture).
    inline bool DecodeTag(const bool (&white)[kTagCells], std::uint32_t& tag)
    {
        if (!white[0] || white[kTagCells - 1])
            return false;
        tag = 0;
        for (std::uint32_t bit = 0; bit < replay::exporting::kTagBits; ++bit)
            tag |= (white[bit + 1] ? 1u : 0u) << bit;
        return true;
    }

    struct TickStamp
    {
        std::atomic<std::uint64_t> serial{~0ull}; // written last: the stamp is valid when it equals the wanted serial
        std::uint32_t phase = 0; // exporting::Phase
        std::uint32_t sample = 0;
        std::uint32_t samples = 1;
        std::uint32_t flags = 0; // kFirstOfFrame | kLastOfFrame | kTagged
        std::int64_t frame = 0;
        std::int64_t timeNs = 0;
        float weight = 0.0f;
        std::uint32_t playheadMs = 0;
    };

    constexpr std::uint32_t kFirstOfFrame = 1u << 0;
    constexpr std::uint32_t kLastOfFrame = 1u << 1;
    constexpr std::uint32_t kTagged = 1u << 2;

    struct Channel
    {
        std::uint32_t magic = kMagic;
        std::uint32_t version = kVersion;
        std::uint32_t bytes = 0; // sizeof(Channel) of the probe that made it
        std::atomic<std::uint32_t> state{0};      // State
        std::atomic<std::uint32_t> requestSeq{0}; // the editor bumps it after writing settings
        std::atomic<std::uint32_t> cancel{0};     // either side asks to stop
        Settings settings;
        // Probe -> add-on.
        std::atomic<std::uint64_t> probeSerial{0};   // ticks written so far
        std::atomic<std::uint64_t> framesTotalHint{0}; // estimated output frames (for the progress bar)
        std::atomic<std::uint32_t> timerLocked{0};   // 1: the game timer follows the export clock (else the export is refused)
        TickStamp ticks[kTickRing];
        // v2, probe -> add-on: the audio pass window (QPC counts) and the
        // timer's health for the dialog.
        std::atomic<std::uint64_t> audioStartQpc{0}; // the tick the playhead left settings.startMs at 1x
        std::atomic<std::uint64_t> audioEndQpc{0};   // the tick it passed settings.endMs
        std::atomic<std::int64_t> timerErrorTicks{0};
        std::atomic<std::uint32_t> frameTimeMismatches{0}; // GET_FRAME_TIME != planned step x warp
        // Add-on -> probe.
        std::atomic<std::uint32_t> addonReady{0};
        std::atomic<std::uint32_t> latencyPresents{~0u}; // measured from the preroll tags (~0 = not yet)
        std::atomic<std::uint64_t> presents{0};
        std::atomic<std::uint64_t> framesAccumulated{0};
        std::atomic<std::uint64_t> framesEncoded{0};
        std::atomic<std::uint32_t> queueDepth{0};
        std::atomic<std::uint32_t> presentWaitUs{0}; // the last present's wait on the encoder queue
        std::atomic<std::uint32_t> error{0};         // editor/render_export/export_types.h ExportError
        // v2, add-on -> probe: stage timings (us, running average over the
        // last frames) for the probe's RENDER_EXPORT log lines.
        std::atomic<std::uint32_t> stageUs[kStageCount] = {};
        std::atomic<std::uint64_t> wallStartQpc{0};  // first Capture picture
        std::atomic<std::uint64_t> bytesWritten{0};  // to the encoder pipe
        // v3, add-on -> probe: AudioAck, the audio pass capture's start.
        std::atomic<std::uint32_t> audioCapture{0};
        // v4, probe -> add-on: a whole project export. segmentCount 0: one
        // range of the clip open (as before). Else this request is piece
        // segmentIndex of segmentCount: the add-on keeps its video and sound
        // (SegmentDone) and joins them all after the last one into
        // settings.outputPath. projectFramesBefore: the frames of the pieces
        // done; projectFramesTotal: the whole's estimate (progress only).
        std::atomic<std::uint32_t> segmentIndex{0};
        std::atomic<std::uint32_t> segmentCount{0};
        std::atomic<std::uint64_t> projectFramesBefore{0};
        std::atomic<std::uint64_t> projectFramesTotal{0};
    };

    // Local\ReplayExportChannel-<pid>.
    inline void Name(wchar_t (&out)[64], unsigned long processId)
    {
        const wchar_t prefix[] = L"Local\\ReplayExportChannel-";
        int n = 0;
        for (; prefix[n]; ++n)
            out[n] = prefix[n];
        wchar_t digits[16];
        int d = 0;
        do
        {
            digits[d++] = static_cast<wchar_t>(L'0' + processId % 10);
            processId /= 10;
        } while (processId && d < 15);
        while (d)
            out[n++] = digits[--d];
        out[n] = 0;
    }

    // Writer side (probe): one stamp per tick; serial last so a reader never
    // takes a half written stamp.
    inline void WriteStamp(Channel& channel, const replay::exporting::Tick& tick, std::uint32_t playheadMs, bool tagged)
    {
        TickStamp& stamp = channel.ticks[tick.serial % kTickRing];
        stamp.serial.store(~0ull, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        stamp.phase = static_cast<std::uint32_t>(tick.phase);
        stamp.sample = tick.sample;
        stamp.samples = tick.samples;
        stamp.flags = (tick.firstOfFrame ? kFirstOfFrame : 0u) | (tick.lastOfFrame ? kLastOfFrame : 0u) | (tagged ? kTagged : 0u);
        stamp.frame = tick.frame;
        stamp.timeNs = tick.timeNs;
        stamp.weight = tick.weight;
        stamp.playheadMs = playheadMs;
        stamp.serial.store(tick.serial, std::memory_order_release);
        channel.probeSerial.store(tick.serial + 1, std::memory_order_release);
    }

    // Reader side (add-on): false when the stamp of `serial` is not there
    // (not written yet, or overwritten: the add-on fell kTickRing behind).
    inline bool ReadStamp(const Channel& channel, std::uint64_t serial, TickStamp& out)
    {
        const TickStamp& stamp = channel.ticks[serial % kTickRing];
        if (stamp.serial.load(std::memory_order_acquire) != serial)
            return false;
        out.phase = stamp.phase;
        out.sample = stamp.sample;
        out.samples = stamp.samples;
        out.flags = stamp.flags;
        out.frame = stamp.frame;
        out.timeNs = stamp.timeNs;
        out.weight = stamp.weight;
        out.playheadMs = stamp.playheadMs;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (stamp.serial.load(std::memory_order_relaxed) != serial)
            return false;
        out.serial.store(serial, std::memory_order_relaxed);
        return true;
    }
}
