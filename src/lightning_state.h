#pragma once

// The engine side of the lightning track (lightning_track.h): guarded reads of
// the weather manager's lightning fields, so the recorder can see every
// lightning the game starts. Addresses come from the game's own code after a
// signature check (build 1491.50, docs/research/recording-capture-probe.md).
//
// Personal build: nothing in the shipped editor reads lightning; the capture
// probe (developer build) proved the offsets this uses. A mismatch disables the
// track (logged once by the caller); nothing is guessed. Only the script thread
// calls these.

#include <cstdint>
#include <string>

namespace replay::lightningstate
{
    // The manager's state as one frame shows it. The type stays non-zero for
    // several frames while a lightning runs; a new lightning is a type that
    // left zero or changed.
    struct Snapshot
    {
        std::int32_t type = 0; // 0 none, 1 directional, 2 cloud, 3 strike
        bool request = false;   // the manager's request flag (+0x6DE0): set by the force native
        bool positionKnown = false;
        float position[3] = {}; // the strike's world position, when the manager wrote one
    };

    // Signature checks, once per module load. False (and why) on a mismatch.
    bool Resolve(std::string& error);
    bool Ready();
    // The manager's fields now; false when unreadable.
    bool Read(Snapshot& out);
}
