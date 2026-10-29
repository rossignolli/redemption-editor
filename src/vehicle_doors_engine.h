#pragma once

// The engine side of vehicle_doors.h: the door ratios of a recorded
// vehicle or train car read from the engine, and put back on the puppet.
//
// Build 1491.50, offline analysis:
// - IS_VEHICLE_DOOR_FULLY_OPEN's core (0x10EC3FC) maps the script door index
//   through a table of hierarchy ids (lea at 0x10EC418, .data) and finds the
//   door with 0x1596A6C: the vehicle's doors are an array at [veh+0xBD0],
//   count s32 [veh+0xBD8], 0x70 bytes each, hierarchy id s16 at +0xC; the
//   door is "fully open" when the float at +0x28 is above 0.99 (flags
//   +0x14, target +0x20, old target +0x24, current +0x28;
//   SetTargetDoorOpenRatio 0x14D7C24 stores +0x24, +0x20 and tests bit 18
//   of +0x14, IS_ARTICULATED).
// - SET_VEHICLE_DOOR_CONTROL (0x10F3C10) takes vehicle types 0..11 and the
//   trains (17..19) and sets the target with flags 0x4200; in this build the
//   setter then steps the current ratio toward it (no instant set), so the
//   puppet's door can lag: the readback decides.
// - _0x762FDC4C19E5A981 (0x10F2848, train types 17 and 18 only) ends in
//   0x1545E88, which stores its bool at [car+0x1680] (0x1545F6F); the
//   decompiled train holdup scripts call it on the boxcar and refrigerator
//   cars with the door scenes. Recorded and played as the latch channel.
//
// Rungs: the reads are rung 2 (layout proven above, signatures checked once,
// SEH); the play drives each door with SET_VEHICLE_DOOR_CONTROL (rung 1)
// and, when the readback is still off, writes the current ratio of the
// puppet's own door (rung 3, ini VehicleDoorWrite, default 1).

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
