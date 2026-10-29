#pragma once

// The volume probe, engine side. A measurement build: in the recording and in
// the replay, every VolumeProbeSampleMs of wall time it walks the engine's
// sound pool, reads every audEnvironmentSound (one per voice that plays) and
// splits its final gain into the engine's stages (requested and combined
// volume, post-submix, resolved category volume, mix groups + occlusion +
// distance, distance to the volume listener), classifies the voice by what
// made its top-level sound (speech line type/source, census caller family,
// recorded owner) and sums it per class and per engine category over windows
// of VolumeProbeWindowMs of clip time.
//
// Engine access: rung 2 only (guarded reads of fields proven offline,
// volume_probe_core.h kSites, checked byte for byte once; a mismatch disables
// the probe and logs VOLUME_PROBE_CHECK,ok=0,site=...), plus natives that read
// (positions, the rendered camera, AUDIO_IS_MUSIC_PLAYING). No detour, no call
// into the engine, no write. Script thread only. Every read refuses an address
// below 4 GB without touching it (guarded_memory.h kGameHeapLowest), and the
// bucket stride is a site. post_db is the post the chain applied (the combine
// buffer's, 0 on a skipping bus), the speech sounds are read right before each
// snapshot, born rows age out, a voice list that moves under the walk is
// walked again (list_retried, list_growing), and a pass ends with its
// recording or playback.
//
// The walk follows each bucket's voice list (the list the audio frame
// processes: every voice, not only the top-level environment sounds). An
// entry of the voice list is kept when its sound's E+0x242 is the entry
// (registered, not mixed yet) or 0xFFFF (mixed: the frame's metric builder
// resets it, 0x256D831); a sound slot counts once per bucket walk (list_dup).
// The global audio entities' voices get classes of their own (wind, weather,
// ambient, emitter, ptfx, script, music; hoof and horse from the census); the
// chain's residual is split into the mix group term and the mixer scene term
// (rest = occlusion + distance); the quad speaker levels of the unpositioned
// beds are read, and the controls of the wind and the speech duck (with its
// weatherApply and sfxApply) logged per window. quad_index, env_group and
// posmode_set are read off the prologues the sound port detours hook.
//
// Lines: VOLUME_PROBE_CHECK (once; ok=0,part=... turns one optional part
// off), VOLUME_PROBE (per closed window, one per class with voices and one
// "all"), VOLUME_PROBE_CATEGORY (per window, the loudest
// VolumeProbeCategories categories), VOLUME_PROBE_WORLD (per window, the
// loudest VolumeProbeWorld groups of world-side voices by entity, top type
// and category), VOLUME_PROBE_STATE (per window: snapshots, voices,
// listener, music, sync byte, cost, the walk's evidence), VOLUME_PROBE_ENV
// (with VolumeProbeEnv=1: weather and wind controls, interior, exposure,
// speech duck, audio flags, music, mixer scenes below unity),
// VOLUME_PROBE_TOP (with VolumeProbeTop=1: the loudest voice of each class
// per window), VOLUME_PROBE_TOTAL (per pass). Every window line carries
// seg= (a new segment after a loop or seek back). tools/volume_probe_diff.py
// reads them.
//
// Ini: VolumeProbe (0), VolumeProbeSampleMs (100), VolumeProbeWindowMs (500),
// VolumeProbeCategories (6), VolumeProbeTop (1), VolumeProbeWorld (8),
// VolumeProbeEnv (1).

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
