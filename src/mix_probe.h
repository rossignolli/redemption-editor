#pragma once

// The mix probe, engine side. Read only: natives that read and guarded reads of
// fields proven offline (ped audio holder LOD +0x45E8, distance metric +0x4640,
// component row player bit +0xB8, mount link slot +0xA8). No detour, no write.
// Each census row with a recorded owner becomes a sample (mix_probe_core.h); the
// MIX_* lines and ReplayDebug\<clip>.sound-mix.txt compare a playback with its
// recording, MIX_MARK covers the 3 s before each QA mark. Ini: MixProbe (0).

#include "replay_context.h"

#include <string>

namespace replay::mixprobe
{
#if defined(REDEMPTION_DEV)
    void SetEnabled(bool enabled);
    bool Enabled();
    // Script thread, right after census::Tick (its rows are this tick's).
    void Tick(Context& context, const char* phase, double clipMs, bool normal);
    // Script thread, every frame: closes the running pass after a gap.
    void Idle(Context& context);
    // A playback starts (after census::BeginPlayback).
    void BeginPlayback(Context& context);
    // A playback ends: closes the pass, compares with the clip's recording
    // samples, writes ReplayDebug\<clip>.sound-mix.txt and returns the
    // section for the sound-check report ("" when off).
    std::string EndPlayback(Context& context);
#else
    // A research probe, built only in the developer build (REDEMPTION_DEV);
    // the public build has these no-ops, which is what the probe does with
    // its key at 0 (the default).
    inline void SetEnabled(bool) {}
    inline bool Enabled() { return false; }
    inline void Tick(Context&, const char*, double, bool) {}
    inline void Idle(Context&) {}
    inline void BeginPlayback(Context&) {}
    inline std::string EndPlayback(Context&) { return std::string(); }
#endif
}
