#pragma once

// What every subsystem module works on: the clip, the recorder registry,
// the puppet registry, the transport and the log, plus the few playback
// core services a subsystem needs, behind a small interface. Modules keep
// their own private state; nothing here is global.

#include "replay_puppets.h"
#include "replay_recorder.h"
#include "replay_recording.h"
#include "replay_transport.h"

#include <cstdint>
#include <ostream>

namespace replay
{
    struct PlaybackHooks
    {
        virtual ~PlaybackHooks() = default;
        // POSE_V2_PLAYBACK_PUPPET line for a puppet state change.
        virtual void LogPuppet(const playback::Puppet& puppet, const char* status, std::uint32_t timeMs) = 0;
        // The puppet's skeleton may have been rebuilt: re-arm and write its
        // pose before this frame renders.
        virtual void RefreshOverride(playback::Puppet& puppet, std::uint32_t timeMs) = 0;
        // Disarms the puppet's pose override (the pose detour stops writing into its
        // skeleton) before the puppet lets its entity go.
        virtual void ReleaseOverride(playback::Puppet& puppet) = 0;
        // Normal play: the playhead moves forward at +1x (not paused, not
        // preparing, not scrubbing). One-shot events play only then.
        virtual bool NormalPlay() const = 0;
    };

    struct Context
    {
        Recording& clip;
        recorder::Recorder& recorder;
        playback::Puppets& puppets;
        transport::Transport& transport;
        std::ostream& log;
        PlaybackHooks* hooks = nullptr;
    };
}
