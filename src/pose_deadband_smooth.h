#pragma once

// Playback smoothing of the pose codec's deadband steps.
//
// The pose stream (pose_stream.h) re-sends a channel only when it moved more
// than the epsilon (0.002 rad, 0.5 mm) from the value the decoder holds, so a
// bone that drifts slowly (an idle, breathing, a face at rest) plays as a
// staircase: held for a few frames, then a jump of about the epsilon, each
// bone at its own frames. Composed down a chain the jumps add up: on a
// smooth animation the codec alone moves an upper body bone 0.7 mm (median),
// 1.7 mm (p90) and 2.9 mm (p99) from one capture frame to the next
// (ReplayClipTool pose-codec, 244-Colter, Abigail), a fine trembling that
// shows when the animation is nearly still; a fast motion hides it, a fully
// still pose has none (nothing is re-sent).
//
// The smoother takes the decoded channel at the frames before and after:
// when the channel moved less than the gate over the two steps (a nearly
// still bone, where the steps are the codec's), it plays the [1 2 1] / 4
// average of the three; otherwise the frame as decoded. A moving bone is not
// touched, a still one is a no-op (three equal values), and the deadband
// staircase becomes ramps. Rotations are averaged in the hemisphere of the
// middle one and normalised; scale and the w lane are left as decoded.
//
// Pure: unit tested in tests/pose_rate_tests.cpp. Playback applies it at
// clip load to the tracks recorded with the coarse codec (PoseDeadbandSmooth);
// the recorder uses the finer codec of PoseDetail (Detail below).

#include "pose_math.h"
#include "pose_skeleton_layout.h"
#include "pose_stream.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace replay::pose::smooth
{
    // The gate in multiples of the stream's epsilons: the codec's steps are
    // up to one epsilon (plus the rotation quantization), so a channel that
    // moved under kGateEpsilons epsilons over two frames is treated as still.
    constexpr float kGateEpsilons = 4.0f;

    struct Gate
    {
        float rotationRad = 0.008f;
        float translationM = 0.002f;

        static Gate For(const StreamConfig& config)
        {
            return {kGateEpsilons * config.rotationEpsilon, kGateEpsilons * config.translationEpsilon};
        }
    };

    inline float AngleOf(const Quat& a, const Quat& b)
    {
        float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        dot = dot < 0.0f ? -dot : dot;
        return dot >= 1.0f ? 0.0f : 2.0f * std::acos(dot);
    }

    inline void SmoothChannel(const BoneQts& prev, const BoneQts& cur, const BoneQts& next, const Gate& gate, BoneQts& out)
    {
        out = cur;
        if (AngleOf(prev.rotation, next.rotation) < gate.rotationRad)
        {
            const auto signOf = [&](const Quat& q) {
                return q.x * cur.rotation.x + q.y * cur.rotation.y + q.z * cur.rotation.z + q.w * cur.rotation.w < 0.0f ? -1.0f
                                                                                                                         : 1.0f;
            };
            const float sp = 0.25f * signOf(prev.rotation);
            const float sn = 0.25f * signOf(next.rotation);
            out.rotation = Normalize(Quat{sp * prev.rotation.x + 0.5f * cur.rotation.x + sn * next.rotation.x,
                sp * prev.rotation.y + 0.5f * cur.rotation.y + sn * next.rotation.y,
                sp * prev.rotation.z + 0.5f * cur.rotation.z + sn * next.rotation.z,
                sp * prev.rotation.w + 0.5f * cur.rotation.w + sn * next.rotation.w});
        }
        float d2 = 0.0f;
        for (int axis = 0; axis < 3; ++axis)
        {
            const float d = next.translation[axis] - prev.translation[axis];
            d2 += d * d;
        }
        if (d2 < gate.translationM * gate.translationM)
        {
            for (int axis = 0; axis < 3; ++axis)
                out.translation[axis] =
                    0.25f * prev.translation[axis] + 0.5f * cur.translation[axis] + 0.25f * next.translation[axis];
        }
    }

    // Every channel of a frame (`count` of them) from the frames around it.
    inline void SmoothDeadband(const BoneQts* prev, const BoneQts* cur, const BoneQts* next, std::size_t count, const Gate& gate,
        BoneQts* out)
    {
        if (!prev || !cur || !next || !out)
            return;
        for (std::size_t n = 0; n < count; ++n)
            SmoothChannel(prev[n], cur[n], next[n], gate, out[n]);
    }

    // The recorder's codec (PoseDetail in RedemptionEditor.ini, Replay
    // Settings: Pose detail). Measured with ReplayClipTool pose-fidelity on
    // 244-Colter and marybeth.rpclip (the recorded animation low passed and
    // encoded again; the frame to frame motion the codec adds to an upper
    // body bone, median / p99):
    //   coarse    0.002 rad, 0.5 mm, 32 bit small bones: 0.47 / 3.2 mm (the old clips)
    //   0.0005 rad, 0.2 mm:                                0.14 / 0.9 mm, +13% bytes
    //   standard  0.0002 rad, 0.1 mm:                      0.075 / 0.32 mm, +21% bytes
    //   maximum   no deadband, 64 bit rotations for every bone: 0.058 / 0.19 mm,
    //             2.2 times the coarse bytes, 1.85 times the standard (zstd 19:
    //             1.6 times); a 256 MB buffer holds 50 s instead of 84 s on
    //             244-Colter, 53 instead of 94 on marybeth
    // The floor left at maximum is the 16 bit translation (0.12 mm steps).
    enum class Detail : std::uint8_t
    {
        Coarse = 0,   // the old codec (dev key PoseCodecFine=0, for an A/B)
        Standard = 1, // the default
        Maximum = 2,
    };
    constexpr float kStandardRotationEpsilon = 0.0002f;
    constexpr float kStandardTranslationEpsilon = 0.0001f;
    constexpr float kCoarseRotationEpsilon = 0.002f;
    constexpr float kCoarseTranslationEpsilon = 0.0005f;

    struct CodecSetting
    {
        float rotationEpsilon = kStandardRotationEpsilon;
        float translationEpsilon = kStandardTranslationEpsilon;
        bool allHighPrecision = false; // 64 bit rotations for every bone
    };
    inline CodecSetting CodecFor(Detail detail)
    {
        switch (detail)
        {
        case Detail::Coarse: return {kCoarseRotationEpsilon, kCoarseTranslationEpsilon, false};
        case Detail::Maximum: return {0.0f, 0.0f, true};
        default: return {};
        }
    }
    // The ini's text: "standard" or "maximum" (anything else: standard).
    inline Detail DetailFromText(const char* text)
    {
        return text && std::strcmp(text, "maximum") == 0 ? Detail::Maximum : Detail::Standard;
    }
    inline const char* DetailText(Detail detail) { return detail == Detail::Maximum ? "maximum" : "standard"; }
    // The recorder's layout for a setting: every rotation 64 bit at maximum.
    inline void ApplyPrecision(const CodecSetting& setting, SkeletonLayout& layout)
    {
        if (setting.allHighPrecision)
            layout.highPrecision.assign(layout.boneCount, std::uint8_t{1});
    }

    // A track recorded with the coarse codec (the old clips): its nearly still
    // bones play the deadband staircase. A track of a finer codec is not.
    inline bool NeedsSmoothing(const StreamConfig& config)
    {
        return config.rotationEpsilon > 0.5f * kCoarseRotationEpsilon ||
            config.translationEpsilon > 0.5f * kCoarseTranslationEpsilon;
    }

    struct SmoothStats
    {
        std::uint32_t frames = 0;
        std::uint32_t smoothedFrames = 0; // frames with both neighbours (no cut, same layout, close in time)
        std::uint32_t worldSpikes = 0;    // one frame matrix spikes repaired (FindWorldSpikes)
        std::size_t bytesIn = 0;
        std::size_t bytesOut = 0;
    };

    // The track's frames smoothed (SmoothDeadband on the parented channels
    // of every frame whose neighbours are in the same run: same layout, no
    // kFrameNoInterpolation into it or out of it, at most kMaxGapMs apart)
    // and encoded again with the standard codec, same times, world matrices,
    // no interpolation flags and keyframe interval. The object space bones
    // (unresolved) keep their decoded values. Streams three frames at a
    // time (a track is never decoded whole). False with `error` when the
    // input does not import; `out` is then untouched. pose_deadband_smooth.cpp.
    constexpr std::uint32_t kMaxGapMs = 100;
    //
    // With `smoothBones` false the bones and the codec are kept (only the
    // world spikes below are repaired).
    bool SmoothTrack(const TrackExport& in, TrackExport& out, SmoothStats& stats, std::string& error, bool smoothBones = true);

    // A one frame spike of the entity matrix: on 244-Colter Abigail's origin
    // goes 66 mm forward at 27638 ms and comes back exactly at 27671 ms, the
    // skeleton not compensating (the whole body jumps for a frame), on a ped
    // standing still for seconds before and after; the recorded data has it,
    // the smoothing does not change it. A frame is a spike when its origin
    // left the frame before by kSpikeMinM or more (or turned kSpikeMinRad),
    // the frame after is back within kSpikeBackM (kSpikeBackRad) of the
    // frame before, the frames around it hold still (kSpikeStillM) and all
    // of them are close in time with no cut. A real motion that leaves and
    // comes back to the millimetre in one frame, between still frames, does
    // not happen; the repaired frame takes the matrix of its nearer
    // neighbour (at most kSpikeBackM away from either).
    constexpr float kSpikeMinM = 0.02f;
    constexpr float kSpikeMinRad = 0.035f; // 2 degrees
    constexpr float kSpikeBackM = 0.002f;
    constexpr float kSpikeBackRad = 0.0035f;
    constexpr float kSpikeStillM = 0.005f;

    struct WorldFrame
    {
        std::uint32_t timeMs = 0;
        std::uint16_t flags = 0;
        float world[16] = {};
    };
    // The spike frames (indices into `frames`, in order).
    std::vector<std::size_t> FindWorldSpikes(const std::vector<WorldFrame>& frames);
    // The frames of a track as recorded (time, flags, matrix), read from the
    // records without decoding a bone.
    std::vector<WorldFrame> WorldFrames(const TrackExport& track);

    struct RecordsSummary
    {
        std::uint32_t records = 0;
        std::uint32_t smoothed = 0; // records recorded with the coarse codec, smoothed
        std::uint32_t failed = 0;   // left as they were (SmoothTrack refused them)
        std::uint64_t frames = 0;
        std::uint64_t smoothedFrames = 0;
        std::uint64_t bytesIn = 0;
        std::uint64_t bytesOut = 0;
        std::uint32_t worldSpikes = 0; // one frame matrix spikes repaired
        std::uint32_t spikeRecords = 0; // finer codec records encoded again for a spike alone
    };
    // Every record whose track NeedsSmoothing, smoothed in place (a refused
    // one stays as loaded), and any other with a world spike repaired (its
    // bones and codec kept). Idempotent: a smoothed track has the standard codec
    // and no spike left.
    void SmoothRecords(std::vector<PoseRecordExport>& records, RecordsSummary& summary);

    // The keys: PoseDeadbandSmooth (dev, 1: the clip load smooths the coarse
    // tracks), PoseDetail (public: standard or maximum) and PoseCodecFine
    // (dev, 0: the coarse codec for an A/B); script_config.cpp sets them.
    void SetLoadSmoothing(bool on);
    bool LoadSmoothing();
    // The recorder's setting (script thread writes, the capture worker reads
    // it when a track begins: a change applies to the tracks begun after it).
    void SetDetail(Detail detail);
    Detail CurrentDetail();
}
