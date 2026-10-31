#pragma once

// The volume probe, engine side. A measurement build: in the recording and in
// the replay, every sampleMs (100) it walks the engine's voice lists,
// splits each voice's final gain into the engine's stages, classifies the
// voice by what made its top-level sound and sums it per class and per engine
// category over windows of windowMs (500) of clip time.
//
// Engine access: guarded reads only of fields proven offline (rung 2,
// volume_probe_core.h kSites, checked once; a mismatch disables the probe and
// logs VOLUME_PROBE_CHECK,ok=0), plus natives that read. No detour, no call
// into the engine, no write. Script thread only. Every read refuses an address
// below 4 GB (guarded_memory.h kGameHeapLowest), and a voice list that moves
// under the walk is walked again (list_retried, list_growing).
//
// Lines: VOLUME_PROBE_CHECK, VOLUME_PROBE, VOLUME_PROBE_CATEGORY,
// VOLUME_PROBE_WORLD, VOLUME_PROBE_STATE, VOLUME_PROBE_ENV, VOLUME_PROBE_TOP,
// VOLUME_PROBE_TOTAL; window lines carry seg= (a new segment after a loop or
// seek back). tools/volume_probe_diff.py reads them.
// Research: docs/research/audio-voice-volume.md
//
// Ini: VolumeProbe (0). Samples every 100 ms, windows of 500 ms, 6
// categories, the top voice, 8 world groups and the ENV line (Config).

#include "replay_context.h"

#include <cstdint>

namespace replay::volumeprobe
{
    struct Config
    {
        bool enabled = false;
        std::uint32_t sampleMs = 100;
        std::uint32_t windowMs = 500;
        std::uint32_t categories = 6;
        bool top = true;
        std::uint32_t world = 8; // VOLUME_PROBE_WORLD groups per window
        bool env = true;         // VOLUME_PROBE_ENV per window
    };
#if defined(REDEMPTION_DEV)
    void Configure(const Config& config);
    bool Enabled();

    // Script thread, right after census::Tick (its rows are this tick's):
    // phase "rec" or "play", the clip time, whether the clock runs at the
    // recording's rate.
    void Tick(Context& context, const char* phase, double clipMs, bool normal);
    // Script thread, every frame: closes the open window and pass after a gap.
    void Idle(Context& context);
    void BeginPlayback(Context& context);
    void EndPlayback(Context& context);
#else
    // A research probe, built only in the developer build (REDEMPTION_DEV); the
    // public build has these no-ops, which is what the probe does with its key at
    // 0 (the default).
    inline void Configure(const Config&) {}
    inline bool Enabled() { return false; }
    inline void Tick(Context&, const char*, double, bool) {}
    inline void Idle(Context&) {}
    inline void BeginPlayback(Context&) {}
    inline void EndPlayback(Context&) {}
#endif
}
