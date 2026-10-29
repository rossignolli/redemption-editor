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
// Each track is classified by how playback reads it (replay_events.cpp,
// replay_objects.cpp, replay_ropes.cpp, pose playback):
//
//   Track               Kind       Playback reads it as           Before base
//   ------------------  ---------  -----------------------------  -------------------------
//   shots               one-shot   forward Range only             dropped
//   speechLines         one-shot   forward Range (+ load ahead)   dropped (coreMs/stopMs shifted)
//   footsteps           one-shot   forward Range only             dropped
//   explosions          one-shot   forward Range only             dropped
//   animAudio           one-shot   forward Range only             dropped
//   fx                  one-shot   forward Range only             dropped
//   meleeHits           one-shot   forward Range only             dropped
//   bodyImpacts         one-shot   forward Range only             dropped
//   sounds              one-shot   forward Range only             dropped
//   collisionSounds     one-shot*  started only when its start    dropped when startMs < base
//                                  is crossed in forward play     (*intensity samples cut to end)
//   decals              state      initial pass (0,1] + reconcile folded to 1 ms (a)(b)
//   decalRemoves        state      forward Range + chain removal  folded to 1 ms (b)
//   projDecals          state      forward Range (persistent)     folded to 1 ms
//   surfaces            state      latest per entity at playhead  last per entity, 1 ms
//   gore                state      desired mask + forward Range   folded to 1 ms
//   goreHits            state      desired mask + forward Range   folded to 1 ms
//   blood               state      reconcile (aged) + forward     folded to 1 ms, age += gap (c)
//   fireStarts          state      reconcile on the first tick    folded to 0, initial=true,
//                                                                 initialAge += gap (c)
//   fireStops           state      reconcile + forward Range      folded to 0
//   fireItems           samples    latest per fire at playhead    last per fire at 0
//   fragBreaks          state      count of breaks <= playhead    folded to 0
//   timeScale           state      latest sample at playhead      last at 0
//   strapTracks         state      latest per weapon (else first) last per weapon at 0
//   attachFrames        state      latest per weapon (else first) last per weapon at 0
//   weaponTracks        samples    interpolated, clamped to span  last at 0 (d)
//   gameCamera          samples    interpolated, held at ends     last at 0
//   registeredFx        samples    frame at playhead, null after  track ended: dropped;
//                                  endMs                          else last frame at 0
//   ropes (times)       samples    InSpan / VerticesAt            record ended: dropped;
//                                                                 else last sample at 0
//   ropes (held, slot)  state      latest (per slot) <= playhead  last (per slot) at 0
//   lifecycle           blocks     BlockAt / AliveAt              blocks before base dropped
//   created             creates    census and first create        entities of kept blocks only,
//                                                                 timeMs clamped to 0
//   segments            state      latest started <= playhead     superseded ones dropped, 0
//   pose records        chunks     chunk + frame at playhead      chunks before the base chunk
//                                                                 dropped; frames clamped to 0
//   worldObjects        (no time)  stand-in until the prop        kept for kept props
//   fragPieces          (no time)  the piece made at its creation kept for kept props
//   scenery             life       appear <= t < gone             alive in the window, moved
//   sceneryChildren     life       appear <= t < gone             alive with a kept parent, renumbered
//   environment         clock      interpolated start to end      clock at base and at end
//   environment.sky     samples    interpolated round the period  state at base, at 0
//   environment.timecycle samples  keys held, floats interpolated state at base, at 0
//   environment.moon    samples    interpolated round the turn    state at base, at 0
//   place, area         (no time)                                 kept
//   scorched            state      latest per entity at playhead  last per entity at 0
//   vehicleWindows      state      latest per vehicle at playhead last per vehicle at 0
//   vehicleParts        state      latest per vehicle at playhead last per vehicle at 0
//   vehicleDoors        state      per door, interpolated         last per door at 0
//   vehicleBurnt        state      latest per vehicle at playhead last per vehicle at 0
//   burnLevels          state      latest per entity at playhead  last per entity at 0
//   sceneryBurnLevels   state      latest per entry at playhead   kept entries, renumbered, last at 0
//   objectLooks         state      latest per object at playhead  last per object at 0
//   sceneryObjectLooks  state      latest per entry at playhead   kept entries, renumbered, last at 0
//   fireChars           state      latest per entity at playhead  last per entity at 0
//   appearances         state      latest per entity at playhead, last per entity at 0 when newer
//                                  unless a later segment         than the segment at base
//   world.ipls          state      latest per IPL at playhead     last per IPL at 0, "off" dropped
//   world.entitySets    state      latest per set (else first)    last per set at 0
//   spans               spans      span at playhead (editor)      part inside the window
//   cuts                instants   no interpolation across one    after base only, shifted
//
// (a) The recorder puts the decals alive at record start at 1 ms and
//     playback applies (0, 1] on its first tick; every forward cursor takes
//     (from, to], so an event at 0 would never play. The state tracks that
//     a forward cursor plays are therefore folded to 1 ms, the others to 0.
// (b) A decal add and its remove both before base are dropped together
//     when the match is certain: the engine id appears in exactly one add
//     (as its result, never as a request id) and exactly one remove, and the
//     add comes first. Everything else is folded in order.
// (c) The age playback derives from the event time is kept: a blood record
//     and a fire folded forward by `gap` ms are made `gap` ms older.
//     A decal too (its settings aged, decal_state.h); one expired
//     by then is dropped.
// (d) Kept whenever the entity is kept, even when its samples ended before
//     base: the track clamps to its span, so the last sample is the state.
//
// Tracks keyed by an entity (weapon, strap and attach tracks, pose records,
// segments, surfaces, decals, gore, blood, world objects, frag breaks) are
// dropped with an entity that is not in the kept blocks. Everything after
// toMs is dropped; a pose chunk that spans toMs loses its frames after it
// (the frames are deltas on the earlier ones, so the tail cut is safe).
// Pose frames before base stay in the base chunk (a delta frame cannot be
// removed without re-encoding) with their time clamped to 0: the pose at 0
// is the last frame at or before base.
//
// Session lists (objectsBefore, pickupsBefore, adoptedObjects) and
// fromDisk are copied as they are: a live buffer cut keeps its session.
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
