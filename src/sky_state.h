#pragma once

// The engine side of the sky track (sky_track.h). Rung 2 only: guarded
// reads of the cloud accumulators and of their fmod periods, at addresses
// taken from the game's own code after a signature check (build 1491.50):
//   0x5DE0A0  _SET_CLOUD_NOISE setter:    movaps [0x4078F60], xmm0
//   0x5DE08C  _SET_CLOUD_POSITION setter: movaps [0x3F29D10], xmm0
//   0x5DE074  _SET_CLOUD_LAYER setter:    lea rdx, [0x4054160] (16 bytes a layer, 2 layers)
//   0x5DDFD8  0x085C5B61A0114F32 setter:  fmod(x, [0x330C7A8] = 1.0) -> [0x4054180]
//   0x5DE0AC  0x1FF8731BE1DFC0C0 setter:  fmod(x, max(1, [0x5A86EA0] - [0x5A86E94])) -> [0x3F28FB0]
//   0x5F8640  the sky update: movss xmm6, [0x39AA79C] (position period),
//             movss xmm6, [0x39AA77C] (noise period), lea rcx, [0x40651E0]
//             (the offset with no setter, read for the log only).
// A mismatch disables the track (logged once); nothing is guessed. The
// writes are the natives above, on the script thread (script.cpp).

#include "sky_track.h"

#include <string>

namespace replay::skystate
{
    // Signature checks, once per module load. False (and why) on a mismatch.
    bool Resolve(std::string& error);
    // The accumulators now (timeMs left to the caller).
    bool Read(SkySample& out);
    // The fmod periods (layer and drift: the game's 1.0 constant).
    bool ReadPeriods(SkyPeriods& out);
}
