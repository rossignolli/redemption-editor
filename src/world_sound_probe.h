#pragma once

// The world sound probe, engine side. Read only (guarded reads of fields
// proven offline in the 1491.50 executable, sound_world_core.h kProbeSites
// checked byte for byte once; a mismatch turns that part off and logs
// WORLD_SOUND_PROBE_CHECK,ok=0,part=...). No detour, no call into the engine,
// no write, no native. Script thread only, right after census::Tick (its
// owner table names the group members).
//
// Lines (recording and replay alike, clip_ms on the clock the volume probe
// uses, so tools/volume_probe_diff.py --sounds lines them up):
// - PTFX_AUDIO_SLOT: a particle audio slot of the engine (manager 0x5C9A660)
//   that started, made its sound, was marked done without one (the listener
//   farther than the settings' distance, 0x320E615) or ended; with the slot's
//   max distance and the distance to the nearest listener point;
// - GROUP_AUDIO: each active horse group (manager [0x5BF7EA8]) once per
//   WorldSoundProbeGroupMs of clip time and when its loop starts or stops:
//   members, replay members (puppets in play, recorded entities in rec),
//   movers by gait, mean speed and position, the loop (such as 0xd9388);
// - WORLD_SOUND_PROBE_TOTAL: per pass (a recording or a playback).
//
// Ini: WorldSoundProbe (default: the VolumeProbe value), WorldSoundProbeGroupMs
// (1000). Lines per minute of wall time: 300 PTFX_AUDIO_SLOT, 600 GROUP_AUDIO
// (the rest counted as lines_dropped).

#include "replay_context.h"

#include <cstdint>

namespace replay::worldsoundprobe
{
    struct Config
    {
        bool enabled = true;
        std::uint32_t groupMs = 1000;
    };
#if defined(REDEMPTION_DEV)
    void Configure(const Config& config);
    bool Enabled();

    // Script thread, right after census::Tick: phase "rec" or "play", the
    // clip time, whether the clock runs at the recording's rate.
    void Tick(Context& context, const char* phase, double clipMs, bool normal);
    // Script thread, every frame: closes a pass after a gap.
    void Idle(Context& context);
    void EndPlayback(Context& context);
#else
    // A research probe, built only in the developer build (REDEMPTION_DEV); the
    // public build has these no-ops, which is what the probe does with its
    // key at 0 (the default).
    inline void Configure(const Config&) {}
    inline bool Enabled() { return false; }
    inline void Tick(Context&, const char*, double, bool) {}
    inline void Idle(Context&) {}
    inline void EndPlayback(Context&) {}
#endif
}
