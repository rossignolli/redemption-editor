#pragma once

// The mixer scenes and the audio mix groups, engine side (mix_scene_core.h
// has the pure parts). Script thread only.
//
// Recording: the theatre's scene start and stop detours
// (TheatreHooks bits 2 and 16) feed the scene events; each tick they become
// the clip's MIXS instances, and 4 times a second the recorded peds' mix
// groups (_GET_ENTITY_AUDIO_MIX_GROUP, rung 1) become its group samples.
//
// Playback: the engine's scenes are followed and logged (MIX_SCENE_PLAYBACK,
// MIX_SCENE_POOL); the speech duck and the category state read the recorded
// track. Putting the recorded scenes and groups back (MixSceneReplay and its
// keys) left.

#include "replay_context.h"

#include <cstdint>
#include <string>
#include <vector>

namespace replay::mixscene
{
    struct Config
    {
        bool probe = false; // SpeechLevelProbe (wants the live scenes)
    };
    void Configure(const Config& config);
    const Config& GetConfig();

    // Every frame (both phases and idle): the scene events into the live set.
    void Idle(Context& context);
    // Recording tick (after census::Tick): events and groups into the clip.
    void Record(Context& context, std::uint32_t nowMs);
    void BeginPlayback(Context& context);
    void Play(Context& context, std::uint32_t timeMs);
    void EndPlayback(Context& context);

    // For the speech level probe: the engine's scenes active now.
    std::size_t OpenCount();
    // A conversation scene active now acts on this mix group (the group is
    // not one of its participants'); its hash in `sceneHash`.
    bool ConversationActsOn(std::uint32_t group, std::uint32_t& sceneHash);
    // "0x5b9de6b4:conversation:tags=2|0x...:code" ("none").
    std::string OpenText();
}
