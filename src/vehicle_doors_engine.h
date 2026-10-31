#pragma once

// The engine side of vehicle_doors.h: the door ratios of a recorded
// vehicle or train car read from the engine, and put back on the puppet.
// Also the train car door latch (the bool the train holdup scripts set on
// boxcars and refrigerator cars), recorded and played as the latch channel.
//
// SET_VEHICLE_DOOR_CONTROL only sets a target that the engine steps toward
// (no instant set), so the puppet's door can lag: the readback decides.
//
// Rungs: the reads are rung 2 (offline dump of build 1491.50, signatures
// checked once, SEH); the play drives each door with SET_VEHICLE_DOOR_CONTROL
// (rung 1) and, when the readback is still off, writes the current ratio of
// the puppet's own door (rung 3, ini VehicleDoorWrite, default 1).
// Research: docs/research/vehicles-doors.md

#include "vehicle_doors.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace replay::vehdoors
{
    // The signatures, once; one log line.
    bool Check(std::string& error);
    std::string Describe();
    void SetWrite(bool on);

    // Recording: one vehicle read into its track (rate gated per vehicle,
    // kSampleMs). `train` reads the latch channel too.
    void BeginRecording();
    void RecordVehicle(std::vector<DoorTrack>& tracks, std::uint32_t id, std::int32_t handle, bool train,
        std::uint32_t timeMs, std::ostream& log);
    void EndRecording(const std::vector<DoorTrack>& tracks, std::ostream& log);

    // Playback: the record at the playhead on a puppet (vehicle kind), the
    // readback judged. One summary line at the end.
    void BeginPlayback(const std::vector<DoorTrack>& tracks);
    const DoorTrack* TrackOf(const std::vector<DoorTrack>& tracks, std::uint32_t id);
    void Apply(std::int32_t clone, const DoorTrack& track, std::uint32_t timeMs, std::ostream& log);
    void EndPlayback(std::ostream& log);

    // Bench: one door's ratio (false when the vehicle has no such door or
    // the layout is not proven).
    bool ReadDoorRatio(std::int32_t vehicle, int door, float& ratio);
}
