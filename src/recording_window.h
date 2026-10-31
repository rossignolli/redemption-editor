#pragma once

// Recording window: cuts a recording to [fromMs, toMs] the way a replay
// editor cuts its replay ring (any lifecycle block can be the first block
// of a clip). Used to save "the last N seconds" of an always-on buffer and
// to trim clips offline (ReplayClipTool trim).
//
// The clip starts at the base: the start of the lifecycle block that holds
// fromMs (each block begins with the full create and attach state of every
// live entity). Every time in the output is shifted by -base; everything
// after toMs goes (duration = toMs - base). Ids are kept as they are.
//
// Each track is cut by how playback reads it: one-shot events before base
// are dropped; state and sample tracks are folded to their last value at 0
// (to 1 ms for the tracks a forward cursor plays, since a cursor takes
// (from, to] and would never play an event at 0). Tracks keyed by an entity
// go with an entity that is not in the kept blocks. Pose frames before base
// stay in the base chunk with their time clamped to 0, since a delta frame
// cannot be removed without re-encoding. Session lists and fromDisk are
// copied as they are. The per-track table and the folding rules:
// Research: docs/research/recording-window.md
//
// Pure data: no engine or native access, unit tested in ReplayCoreTests.

#include "camera_track.h"
#include "pose_stream.h"
#include "replay_recording.h"

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::window
{
    // How many of one kind a recording holds (events, samples, records).
    struct KindCount
    {
        const char* name = "";
        std::uint64_t before = 0;
        std::uint64_t after = 0;
    };

    struct Result
    {
        std::uint32_t baseMs = 0;     // input time of the output's 0
        std::uint32_t endMs = 0;      // input time of the output's end
        std::uint32_t durationMs = 0; // endMs - baseMs: the output's duration
        std::vector<KindCount> kinds; // per kind, input and output
        std::uint64_t decalPairsDropped = 0;
        std::uint64_t decalsExpiredDropped = 0; // aged past their life by base
        std::uint64_t poseFramesClamped = 0;   // frames before base kept at 0
        std::uint64_t poseFramesTruncated = 0; // frames after end cut from a chunk
        std::string error;
    };

    // The base Cut would use for fromMs: the start of the lifecycle block
    // holding it (0 before the first block).
    std::uint32_t BaseFor(const Recording& recording, std::uint32_t fromMs);

    // Cuts `in` (with its pose records, as clipfile::Save and
    // pose::capture::ExportRecords pass them, and its duration) to
    // [fromMs, toMs]; toMs is clamped to durationMs. `out` and `outPoses`
    // are replaced (they must not be the inputs). False with result.error
    // when the window is empty or a pose record is malformed; the outputs
    // are then unspecified.
    bool Cut(
        const Recording& in,
        const std::vector<pose::PoseRecordExport>& inPoses,
        std::uint64_t durationMs,
        std::uint32_t fromMs,
        std::uint32_t toMs,
        Recording& out,
        std::vector<pose::PoseRecordExport>& outPoses,
        Result& result);

    // Editor camera keyframes inside [baseMs, endMs], shifted by -baseMs
    // (ids kept); the ones outside are dropped.
    void CutCamera(const camera::Track& in, std::uint32_t baseMs, std::uint32_t endMs, camera::Track& out);

    // Where the player was over a running buffer, so a saved window is named
    // (and its PLAC written) after the place where the window starts, not where
    // the buffer started. One sample per change of the map zones, kept from the
    // ring's head like the other tracks.
    struct PlaceSample
    {
        std::uint32_t timeMs = 0;
        Place place;
    };

    inline bool SamePlaceZones(const Place& a, const Place& b)
    {
        return a.zoneTown == b.zoneTown && a.zoneDistrict == b.zoneDistrict && a.zoneState == b.zoneState;
    }

    // Adds a sample when the zones differ from the last one.
    inline bool AddPlaceSample(std::vector<PlaceSample>& track, std::uint32_t timeMs, const Place& place)
    {
        if (!track.empty() && (SamePlaceZones(track.back().place, place) || timeMs < track.back().timeMs))
            return false;
        track.push_back(PlaceSample{timeMs, place});
        return true;
    }

    // The sample in force at ms (the last at or before it, else the first);
    // null for an empty track.
    inline const Place* PlaceAt(const std::vector<PlaceSample>& track, std::uint32_t ms)
    {
        if (track.empty())
            return nullptr;
        const Place* found = &track.front().place;
        for (const PlaceSample& sample : track)
        {
            if (sample.timeMs > ms)
                break;
            found = &sample.place;
        }
        return found;
    }

    // Drops the samples before ms, keeping the one in force there.
    inline void DropPlacesBefore(std::vector<PlaceSample>& track, std::uint32_t ms)
    {
        std::size_t keep = 0;
        while (keep + 1 < track.size() && track[keep + 1].timeMs <= ms)
            ++keep;
        if (keep)
            track.erase(track.begin(), track.begin() + static_cast<std::ptrdiff_t>(keep));
    }

    // The window's place: the zone names in force at fromMs over the cut's
    // own (character and weather stay the cut's).
    inline void ApplyWindowPlace(Place& out, const std::vector<PlaceSample>& track, std::uint32_t fromMs)
    {
        const Place* at = PlaceAt(track, fromMs);
        if (!at || !at->recorded)
            return;
        out.recorded = true;
        out.zoneTown = at->zoneTown;
        out.zoneDistrict = at->zoneDistrict;
        out.zoneState = at->zoneState;
        std::memcpy(out.town, at->town, sizeof(out.town));
        std::memcpy(out.district, at->district, sizeof(out.district));
        std::memcpy(out.state, at->state, sizeof(out.state));
    }

    // Counts per kind (the `before` field), for logs and the clip tool.
    std::vector<KindCount> Tally(const Recording& recording, const std::vector<pose::PoseRecordExport>& poses);
}
