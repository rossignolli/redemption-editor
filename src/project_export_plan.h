#pragma once

// Whole project export: the arithmetic of a video made of the project's
// clips one after the other (replay_project.h ExportPlan gives the pieces).
// Each piece is exported as its own video pass, at its markers' speeds, and
// the pieces are joined in one file: piece k's first frame is the frame
// after piece k-1's last, and its sound is cut to exactly the length of its
// frames, so the seams keep the picture and the sound together.
//
// Pure (unit tested in ReplayCoreTests, tests/project_export_tests.cpp).

#include "export_schedule.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::projectexport
{
    // One piece as the plan needs it: its range in the clip's own time and
    // the speed markers in effect there (exporting::MarkerSpeed, sorted by
    // time; none: 100 %).
    struct SegmentInput
    {
        std::uint64_t startMs = 0;
        std::uint64_t endMs = 0;
        std::vector<replay::exporting::MarkerSpeed> speeds;
    };

    // A piece in the video: its frames and the index of its first frame in
    // the whole video (the frames of the pieces before it).
    struct SegmentFrames
    {
        std::uint64_t startMs = 0;
        std::uint64_t endMs = 0;
        std::uint64_t frames = 0;
        std::uint64_t firstFrame = 0;
    };

    // The speed the playhead runs at `ms` (the last marker at or before it).
    inline double SpeedAt(const std::vector<replay::exporting::MarkerSpeed>& speeds, std::uint64_t ms)
    {
        double speed = 1.0;
        for (const replay::exporting::MarkerSpeed& marker : speeds)
        {
            if (marker.timeMs > ms)
                break;
            speed = marker.speed;
        }
        return speed > 0.0 ? speed : 1.0;
    }

    // Each piece's frames at its markers' speeds (as exportprobe::Request
    // counts them: exporting::SpanOutputFrames), and where it starts.
    inline std::vector<SegmentFrames> PlanFrames(const std::vector<SegmentInput>& segments, replay::exporting::FrameRate rate)
    {
        std::vector<SegmentFrames> plan;
        plan.reserve(segments.size());
        std::uint64_t first = 0;
        for (const SegmentInput& segment : segments)
        {
            SegmentFrames out;
            out.startMs = segment.startMs;
            out.endMs = segment.endMs;
            out.frames = replay::exporting::SpanOutputFrames(segment.startMs, segment.endMs, rate,
                [&segment](std::uint64_t ms) { return SpeedAt(segment.speeds, ms); }, 50000000ull);
            out.firstFrame = first;
            first += out.frames;
            plan.push_back(out);
        }
        return plan;
    }

    inline std::uint64_t TotalFrames(const std::vector<SegmentFrames>& plan)
    {
        return plan.empty() ? 0 : plan.back().firstFrame + plan.back().frames;
    }

    // The output time of frame `frame` of the whole video (ns): the frames
    // of the pieces before it at the export's rate, nothing else (no gap, no
    // overlap at a seam).
    inline std::int64_t FrameTimeNs(replay::exporting::FrameRate rate, std::uint64_t frame)
    {
        return replay::exporting::FrameStartNs(rate, static_cast<std::int64_t>(frame));
    }

    // The audio sample (at `sampleRate`) where frame `frame` of the whole
    // video starts: floor(frame / fps * rate), exact in integers.
    inline std::uint64_t AudioSampleAt(std::uint64_t frame, replay::exporting::FrameRate rate, std::uint32_t sampleRate)
    {
        if (!replay::exporting::ValidRate(rate))
            return 0;
        const std::uint64_t perNum = static_cast<std::uint64_t>(sampleRate) * rate.den; // samples in `num` frames
        return frame / rate.num * perNum + frame % rate.num * perNum / rate.num;
    }

    // The samples of a piece's sound: from its first frame's sample to the
    // next piece's. The pieces' counts add up to the whole video's samples,
    // whatever the rate (no rounding drift over many seams).
    inline std::uint64_t AudioSamples(std::uint64_t firstFrame, std::uint64_t frames, replay::exporting::FrameRate rate,
        std::uint32_t sampleRate)
    {
        return AudioSampleAt(firstFrame + frames, rate, sampleRate) - AudioSampleAt(firstFrame, rate, sampleRate);
    }

    // The progress of a project export as a whole: the frames of the pieces
    // done, the current piece's frames encoded, against the total (the pieces
    // done as they came out, the current one's count, the rest planned).
    inline double OverallFraction(std::uint64_t framesBefore, std::uint64_t framesNow, std::uint64_t framesTotal)
    {
        if (framesTotal == 0)
            return 0.0;
        const double fraction = static_cast<double>(framesBefore + framesNow) / static_cast<double>(framesTotal);
        return fraction < 0.0 ? 0.0 : fraction > 1.0 ? 1.0 : fraction;
    }

    // The whole's frames once piece `index` started: the pieces before it as
    // they came out (`framesBefore`), its own count (`currentFrames`) and the
    // plan's for the ones after it.
    inline std::uint64_t TotalEstimate(const std::vector<SegmentFrames>& plan, std::size_t index, std::uint64_t framesBefore,
        std::uint64_t currentFrames)
    {
        std::uint64_t total = framesBefore + currentFrames;
        for (std::size_t k = index + 1; k < plan.size(); ++k)
            total += plan[k].frames;
        return total;
    }

    // The piece holding the whole video's middle frame (the thumbnail's).
    inline std::size_t ThumbnailSegment(const std::vector<SegmentFrames>& plan)
    {
        const std::uint64_t middle = TotalFrames(plan) / 2;
        for (std::size_t k = 0; k < plan.size(); ++k)
            if (middle < plan[k].firstFrame + plan[k].frames)
                return k;
        return plan.empty() ? 0 : plan.size() - 1;
    }

    // The dialog's whole percent (0..100, rounded down: 100 only at the end).
    inline int PercentOf(double fraction)
    {
        const int percent = static_cast<int>(fraction * 100.0);
        return percent < 0 ? 0 : percent > 100 ? 100 : percent;
    }
}
