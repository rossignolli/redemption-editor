#pragma once

// The mixer scenes and the audio mix groups, engine side (mix_scene_core.h
// has the pure parts). Script thread only.
//
// Recording (MixSceneRecord): the theatre's scene start and stop detours
// (TheatreHooks bits 2 and 16) feed the scene events; each tick they become
// the clip's MIXS instances, and 4 times a second the recorded peds' mix
// groups (_GET_ENTITY_AUDIO_MIX_GROUP, rung 1) become its group samples.
//
// Playback (MixSceneReplay): the puppets get the recorded mix groups
// (0x2EE52E8, the internal of ADD_ENTITY_TO_AUDIO_MIX_GROUP, by hash, rung 4;
// REMOVE_ENTITY_FROM_AUDIO_MIX_GROUP, rung 1), the recorded conversation
// scenes start again on a conversation object of the replay's own (0x2FC2200
// start, 0x2FC2964 stop, rung 4) with the recorded participant tags, and the
// recorded script scenes by hash (natives 0xDC2F83A0612CA34D and
// 0x6AB944DF68B512D3, rung 1). The code scenes the engine does not start
// again by itself go straight into the mixer (0x2F19018, rung 4,
// MixSceneReplayCode), and the recorded scene variables are set on the
// replayed scenes (0x3279628, rung 4). Everything stops and the groups come
// off when the playback ends.

#include "replay_context.h"

#include <cstdint>
#include <string>
#include <vector>

namespace replay::mixscene
{
    struct Config
    {
        bool record = true;  // MixSceneRecord
        bool replay = false; // MixSceneReplay
        bool probe = false;  // SpeechLevelProbe (wants the live scenes)
        // The kinds the playback starts again (kReplayScript 1, kReplayConversation 2,
        // kReplayCode 4; MixSceneReplayScript 1, MixSceneReplayConversation 1,
        // MixSceneReplayCode 0), the scene variables recorded and set again
        // (MixSceneVars 1) and the code scene hashes never replayed
        // (MixSceneCodeSkip, a list).
        std::uint8_t kinds = 3;
        bool vars = true;
        std::vector<std::uint32_t> skip;
        // The scene pool read directly (mix_scene_pool_core.h). The recording
        // adopts every scene open when it begins and the scenes the engine's
        // drivers start straight into the pool (MixSceneRecordDirect, MIXS
        // v3); the playback takes the engine's open scenes from the pool, not
        // from the starts the mod saw (MixSceneEngineOpenFromPool), owns the
        // hidden live player's state scenes the clip has (stops the engine's,
        // refuses its driver's starts while the replay owns the world and
        // plays the recorded ones: MixSceneOwnPlayerState) and logs the pool
        // once a second (MixScenePoolLog).
        bool poolRecord = true;
        bool engineOpenFromPool = true;
        bool ownPlayerState = true;
        bool poolLog = true;
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
