#pragma once

// Census-level sound capture. While recording, a top-level sound whose owner
// is a recorded vehicle, train car or object and that no other track recorded
// (checked 300 ms later, once those tracks have filed theirs) is kept with its
// ref, init params and where its audio entity sits relative to the owner (clip
// section GSND). A persistent sound alive before its owner entered the
// recording is adopted from the census's live list. Playback starts each one
// on the owner's puppet by ref, unless the puppet made the same sound itself
// around that time: the puppet's own sound wins, nothing doubles.

#include "replay_context.h"
#include "sound_census_core.h" // census::Owner, AudioBase

#include <cstdint>
#include <string>

namespace replay::capture
{
    void BeginRecording(Context& context);
    // After the other tracks of the tick (events::Record).
    void Record(Context& context);
    void EndRecording(Context& context);

    void BeginPlayback(Context& context);
    // After the census tick and the other sound tracks (events::Play).
    void Play(Context& context, std::uint32_t timeMs);
    // Stops every captured sound; returns the line for the sound report.
    std::string EndPlayback(Context& context);
    // A script restart with a playback open: the weak slots were stopped by
    // the guarded path, nothing reported.
    void ForgetPlaybackAfterRestart();

    // Where an audio entity is from its owner (the base the owner's puppet
    // has too). Shared with the generic sound recorder so its create packets
    // carry the same base, field and offset. False when no base of the owner
    // reaches the audio entity.
    bool ResolveAudioBase(std::uint64_t audio, const census::Owner& owner, census::AudioBase& base, std::uint16_t& field,
        std::int32_t& offset);
}
