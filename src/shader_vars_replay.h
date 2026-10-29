#pragma once

// The horses' moving muscles in the replay (shader_vars_core.h): the
// recording keeps each recorded horse's animated shader variables (section
// SHVR) and the playback writes them into the puppet's shader vars
// component before the shader reads them (shader_vars_hook.h). Script
// thread only.
//
// Lines (every build): MUSCLE_REPLAY_BEGIN / MUSCLE_REPLAY_END (hook,
// section, puppets written, the detour's counts) and MUSCLE_RECORD_END
// (horses and points recorded, read failures). Developer build, the
// evidence of the muscle weights (track 0x17) read where they live:
//   MUSCLE_NM_DOFS,phase=rec|play   once per horse: the component's DOFs
//                                   (nm: track 0x17 DOFs, lanes, histogram)
//   MUSCLE_NM_TICK,phase=rec|play   an A, B, A or a set / unset switch (capped)
//   MUSCLE_NM_CMP,phase=play        every 2 s of clip time per horse: the
//                                   puppet's values against the recording
//   MUSCLE_NM_SUMMARY / MUSCLE_NM_TOTAL at the end of each phase.

#include "replay_context.h"
#include "shader_vars_core.h"

#include <cstdint>

namespace replay::muscles
{
    using Config = replay::shadervars::ReplayConfig; // MuscleRecord, MuscleReplay
    void SetConfig(const Config& config);

    void Attach(Context& context);

    // Recording: one call per capture tick (the recording clock), and the stop.
    void TickRecord(std::uint32_t nowMs);
    void EndRecord();

    // Playback: one call per playback tick (the playhead), and the stop.
    void TickPlay(std::uint32_t timeMs);
    void EndPlay();

    // Outside the replay, every script tick: MuscleDebugForce on the player's
    // horse (developer test of cause; nothing when the key is 0).
    void TickLive(bool playing);

    // Unload: the detour goes.
    void Shutdown();
}
