#pragma once

// The sky object's moon, for the recorded moon track (MoonRecord, MoonReplay;
// moon_track.h). RDR2 has no native for it: the sky object (static at
// 0x4F3E4A0, build 1491.50) computes the sun and the moon every frame from the
// clock in its update 0x145781C (sun +0xA90, moon +0xAA0; the moon's turn
// angle is the double +0xA80, its orbit angle +0x8E8).
//
// Read and written from the script fiber only, with guarded access to the
// signature-checked object; no detour. (1.0 dropped the experimental sun and
// moon override, the SunOverride detour on the update and the SunProbe lines.)

#include <cstdint>

namespace replay::sunctl
{
    struct Vec3f
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    // The moon's two session angles (+0xA80 turn, +0x8E8 orbit) and the drawn
    // directions.
    struct MoonState
    {
        double angle = 0.0; // +0xA80
        double orbit = 0.0; // +0x8E8
        bool directions = false; // sun and moon below read
        Vec3f sun;  // +0xA90 (drawn)
        Vec3f moon; // +0xAA0 (drawn)
    };
    bool ReadMoon(MoonState& out);
    bool WriteMoon(double angle, double orbit);
    // "none", or the site whose signature did not match.
    const char* ResolveError();
}
