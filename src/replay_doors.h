#pragma once

// Map doors in the replay. A door swung by a ped is a map object the recorder
// never lists, so the doors near the scene are found by position in the
// community door table (door_table.h), taken by their door hash, and their
// rotation is recorded when it changes. Playback puts each door at the
// playhead's rotation (frozen, scrubbing back included) and restores it when
// the playback ends. Natives only.

#include "door_rebind.h"
#include "replay_recording.h"

#include <cstdint>
#include <ostream>

namespace replay::doors
{
    void BeginRecording(Recording& clip);
    // Every tick of a recording: finds new doors near `x, y, z` twice a
    // second, samples the watched ones.
    void UpdateRecording(Recording& clip, std::uint32_t nowMs, float x, float y, float z);
    void EndRecording(const Recording& clip, std::ostream& log);

    // The ini's DoorRebindMs, DoorKeepOpenRatio and MapObjectsWaitMs
    // (door_rebind.h).
    void Configure(const Config& config);
    const Config& GetConfig();
    // closedTheatre: the world clear of the closed theater ran (the door
    // system's ratio is kept for a door that goes).
    void BeginPlayback(const Recording& clip, std::ostream& log, bool closedTheatre);
    // Every tick of a playback: the doors whose handle went (or never was)
    // looked for again every rebindMs, each found door played in the tick.
    void UpdatePlayback(const Recording& clip, std::uint32_t playheadMs, std::ostream& log);
    // Moving doors within radiusM of a point still without an object (the
    // start's wait for the map).
    std::uint32_t MissingNear(const Recording& clip, float x, float y, float z, float radiusM);
    void EndPlayback(std::ostream& log);
    // A script restart with a playback open: the played doors are the old
    // world's handles, so they are forgotten, not restored (the load put the
    // world's doors back). Returns the count.
    std::uint32_t ForgetPlaybackAfterRestart();
}
