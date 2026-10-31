#pragma once

// The engine side of the sky track (sky_track.h). Rung 2 only: guarded
// reads of the cloud accumulators and of their fmod periods, at addresses
// taken from the game's own code after a signature check (build 1491.50):
//   _SET_CLOUD_NOISE setter:    movaps [the noise accumulator], xmm0
//   _SET_CLOUD_POSITION setter: movaps [the position accumulator], xmm0
//   _SET_CLOUD_LAYER setter:    lea rdx, [the layer accumulators] (16 bytes a layer, 2 layers)
//   0x085C5B61A0114F32 setter:  fmod(x, [a constant] = 1.0) -> [the drift accumulator]
//   0x1FF8731BE1DFC0C0 setter:  fmod(x, max(1, [the max] - [the min])) -> [the region accumulator]
//   the sky update:             movss xmm6, [the position period],
//                               movss xmm6, [the noise period], lea rcx,
//                               [the unpinned offset] (the offset with no
//                               setter, read for the log only).
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
