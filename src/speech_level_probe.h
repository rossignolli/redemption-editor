#pragma once

// The speech level probe, engine side. Read only: guarded reads of the
// speech lines (fields proven offline, speech_level_core.h) and
// _GET_ENTITY_AUDIO_MIX_GROUP (a native that reads). Ini SpeechLevelProbe
// (0; the QA package sets 1). Lines:
//   SPEECH_LEVEL,phase=record|play,clip_ms,ped,who,line,context,voice,
//     source,type,d8,volume_type,audibility,duck_bit,vol_db,group,
//     conversation_duck,scene,open_scenes
//   SPEECH_LEVEL_TOTAL,phase,...  (per source: samples, mean dB, ducked)
// 4 times a second, each line at first sight in state 5 and every second
// while it plays (4 samples at most per line).

#include "replay_context.h"

namespace replay::speechlevel
{
#if defined(REDEMPTION_DEV)
    void SetEnabled(bool enabled);
    bool Enabled();
    // Script thread: "record" ticks (after census::Tick) and playback ticks.
    void Tick(Context& context, bool play, std::uint32_t clipMs);
    void BeginPlayback(Context& context);
    void EndPlayback(Context& context);
#else
    // A research probe, built only in the developer build (REDEMPTION_DEV);
    // the public build has these no-ops, which is what the probe does with
    // its key at 0 (the default).
    inline void SetEnabled(bool) {}
    inline bool Enabled() { return false; }
    inline void Tick(Context&, bool, std::uint32_t) {}
    inline void BeginPlayback(Context&) {}
    inline void EndPlayback(Context&) {}
#endif
}
