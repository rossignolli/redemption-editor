#pragma once

// The player's horse audio evidence, game side (horse_audio_evidence.h has the
// offline evidence and the pure part). Script thread only. Lines:
//   POSE_V2_HORSE_STEP_AUDIO     one per sound queued on the horse's ped
//                                audio (capped 160 per phase), with the
//                                caller chain, params +0x138 and the inputs
//   POSE_V2_HORSE_AUDIO_STATE    every field, once per second when it changed
//   POSE_V2_HORSE_AUDIO_COMPARE  playback: the fields that differ from the
//                                live horse of the last recording
//   POSE_V2_HORSE_AUDIO_SUMMARY  per phase: refs by caller, steps, diffs

#include "horse_audio_evidence.h"

#include <cstdint>
#include <iosfwd>
#include <string>

namespace replay::horseaudio
{
    // HorseAudioEvidence in the ini (default 0: nothing runs).
    void SetEnabled(bool on);
    bool Enabled();
    // Guarded reads of the fields of horse_audio_evidence.h for a ped.
    void ReadState(void* pedEntity, State& state);
    // phase "record" (the live horse the player rides) or "play" (the
    // puppet horse the player's puppet rides).
    void Begin(const char* phase, std::ostream& log);
    // Every tick of the phase; horse may be null (not mounted).
    void Tick(void* horse, void* rider, const std::string& horseId, std::uint32_t timeMs, std::ostream& log);
    void End(std::ostream& log);
}
