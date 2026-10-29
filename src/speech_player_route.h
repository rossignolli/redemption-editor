#pragma once

// Who says a line of the recorded player.
//
// With SpeechPlayerOnPlayer=1 the recorded player's lines are said by the
// hidden real player, because the engine plays the player's own speech louder
// than the same line said by any other ped. That works because the hidden
// player stands at his puppet: UpdatePlayerFollow (script.cpp) moves him there
// every tick. In a mission the follow is skipped (a mission script may fail on
// distance) and the player stays where the replay was opened: the speech core
// then refuses his lines in state 3 with code 0x29 (the speaker's squared
// distance to the listener over the line's range, 0x2F97C9D -> 0x2F9B3A4 ->
// 0x2F9B31C: 35 or 50 m, 60 m, 100 m, or none by the line's mode). So the
// hidden player says the line only while he is at the puppet; farther, the
// puppet says it.

#include <cstdint>

namespace replay::speech::route
{
    // The follow puts the player (or his horse, with him on it) at the
    // puppet the trail map is centred on: within a metre or two of it.
    constexpr float kNearM = 3.0f;

    enum class Speaker : std::uint8_t
    {
        Puppet,
        Player
    };

    enum class Why : std::uint8_t
    {
        KeyOff,    // SpeechPlayerOnPlayer=0
        NotPlayer, // a line of another speaker
        NoPlayer,  // no player ped to say it
        Far,       // the hidden player is not at the puppet (a mission, a vehicle)
        Near,      // the hidden player stands at the puppet
        Held,      // brought to the puppet for this line (HoldAtPuppet)
        CharacterMismatch // the live player is another character (SameCharacter)
    };

    struct Decision
    {
        Speaker speaker = Speaker::Puppet;
        Why why = Why::KeyOff;
    };

    // The hidden player says the recorded player's lines in HIS voice: the
    // engine resolves the player's current voice for a request with no voice,
    // and even the named pain voices came out as the live character (seen with
    // John's save and a clip recorded as Arthur: Arthur's puppet got
    // shot and played John's pain sounds). So the hidden player stands
    // in only when he is the recorded character: the model of PLAYER_PED_ID now
    // against the recorded player's (the clip's place model, PLAC, the model of
    // PLAYER_PED_ID at record; else his puppet's). An unknown model (0) is not
    // the same character.
    inline bool SameCharacter(std::uint32_t recordedModel, std::uint32_t liveModel)
    {
        return recordedModel != 0 && recordedModel == liveModel;
    }

    // `distanceM` is the hidden player's distance to the speaker's puppet
    // (negative when it could not be read: the puppet says the line).
    // `sameCharacter` is SameCharacter: another character's hidden player never
    // says the line (nor is held for it); his puppet does, with the recorded voice
    // pinned (PinPoseV2Voice) or in the request.
    inline Decision Choose(bool keyOn, bool recordedPlayer, bool playerValid, bool sameCharacter, float distanceM)
    {
        if (!keyOn)
            return {Speaker::Puppet, Why::KeyOff};
        if (!recordedPlayer)
            return {Speaker::Puppet, Why::NotPlayer};
        if (!playerValid)
            return {Speaker::Puppet, Why::NoPlayer};
        if (!sameCharacter)
            return {Speaker::Puppet, Why::CharacterMismatch};
        if (!(distanceM >= 0.0f) || distanceM > kNearM)
            return {Speaker::Puppet, Why::Far};
        return {Speaker::Player, Why::Near};
    }

    inline const char* WhyName(Why why)
    {
        switch (why)
        {
        case Why::KeyOff: return "key_off";
        case Why::NotPlayer: return "not_player";
        case Why::NoPlayer: return "no_player";
        case Why::Far: return "player_far";
        case Why::Near: return "player_near";
        case Why::Held: return "held_at_puppet";
        case Why::CharacterMismatch: return "character_mismatch";
        }
        return "unknown";
    }

    // A line of the recorded player that went to his puppet because the live
    // player is another character never falls back to the puppet's own voice
    // (POSE_V2_SPEECH_OWN_VOICE): that voice is drawn from the model's voice
    // group, not the recorded one. The puppet says it with the recorded voice
    // (pinned, or named in the request) or the line stays silent: better nothing
    // than another character's voice. The player's pain vocals carry no voice
    // object and, on a puppet, are killed while loading (stop reason 4): with
    // another character live they are silent until a puppet can load that
    // character's pain bank (not proved in game).
    inline bool OwnVoiceAllowed(Why why)
    {
        return why != Why::CharacterMismatch;
    }

    // A line routed to the hidden player that the engine did not create on him
    // (or created as another phrase and the replay stopped it before its start)
    // is asked again at once on the puppet. The recorded voice is pinned on the
    // puppet (PinPoseV2Voice), not on the player, and the request carries no
    // voice, so on the player the engine resolves the player's own current voice
    // and can refuse the line with 0x44 (68, the variation check 0x2F9EA2C).
    // `created` is result 74 with a line, `sameLine` that line with the recorded
    // identity, `stoppedOnMismatch` that the replay stopped it (no sound came out).
    inline bool RetryOnPuppet(bool routedToPlayer, bool created, bool sameLine, bool stoppedOnMismatch)
    {
        if (!routedToPlayer || sameLine)
            return false;
        return !created || stoppedOnMismatch;
    }

    // Why the hidden player refuses the recorded player's conversation lines with
    // 68 (build 1491.50):
    //   - the variation step 0x2F9EA2C checks the line against params +0x14C
    //     (voice name hash), +0x150 (context) and +0x158 (variation): with a
    //     variation it calls 0x25317DC(voice, context, variation), and with
    //     the replay's +0x18C set it draws again through 0x2FA844C(voice,
    //     context); no variation of that context in that voice is the 68;
    //   - the voice step 0x2F9EF48 keeps any non-zero +0x14C; with none it
    //     comes from the speaker's voice holder (resolver 0x2FA66D0: holder
    //     +0x30, else a draw from the model's voice group 0x2FA2C54);
    //   - the replay pins the recorded voice on the PUPPET's holder and asks
    //     with no voice, so on the hidden player +0x14C is the player's own
    //     current voice, which may not have the line.
    // Live lines that named their voice are recorded with no voice object (line
    // +0x1E8 = 0). So a line of the recorded player that named its voice is asked
    // on the hidden player with that voice in the request, as the live request
    // did: the variation step finds the line, and the player's own speech
    // treatment is kept. A line recorded with a voice object keeps the holder
    // path (its identity check compares the voice object), and the puppet keeps
    // its pinned voice.
    inline bool NamedVoiceOnPlayer(bool routedToPlayer, bool animal, std::uint32_t voiceHash,
        std::uint32_t recordedVoiceObjectPartial, std::uint8_t recordedVoiceObjectType)
    {
        return routedToPlayer && !animal && voiceHash != 0 && recordedVoiceObjectPartial == 0 &&
            recordedVoiceObjectType == 0;
    }

    // The recorded player's pain, effort and fall vocals (PAIN_HIGH, PAIN_MEDIUM,
    // PAIN_LOW...) are lines that name their voice: the player's pain voices
    // (0xA5D5FD9E, 0xCF175024, 0xF99C4FD0...), recorded with no voice object.
    // Said by the hidden player they play (live evidence, 1268-Blackwater: 9 of 9
    // started, heard at -13.8 dB). Said by the puppet they die while loading
    // (stop reason 4 in state 2, cause killed_loading, QA-232-Valentine and
    // pp-1261-Valentine-p7): the puppet has no wave for the player's pain voice,
    // so the line is silent or, through the own voice retry, another voice.
    // The puppet says them whenever the hidden player is not at it (Why::Far),
    // and the follow puts him at the trail focus, which is not always the
    // recorded player's puppet (the nearest puppet when the player's was not yet
    // shown, kept while it stays within 80 m of the camera).
    // So when the follow is moving the hidden player (not in a mission, not in a
    // vehicle) and such a line is due while he is far, he is brought to the
    // recorded player's puppet and held there while the line plays; the line is
    // then his (Why::Held). A line with a voice object keeps the puppet (its
    // pinned voice plays there), and so does any line while the follow is off.
    // `distanceRead`: the distance behind Why::Far was read (not -1).
    inline bool HoldAtPuppet(const Decision& route, bool distanceRead, bool followMoving, bool animal,
        std::uint32_t voiceHash, std::uint32_t recordedVoiceObjectPartial, std::uint8_t recordedVoiceObjectType)
    {
        return route.speaker == Speaker::Puppet && route.why == Why::Far && distanceRead && followMoving &&
            NamedVoiceOnPlayer(true, animal, voiceHash, recordedVoiceObjectPartial, recordedVoiceObjectType);
    }

    // How long the hold keeps the hidden player at the puppet: at least
    // kHoldMinMs after the request (a grunt is a few hundred ms and the line
    // loads in a frame or two), then while a held line is still alive (created,
    // loading or playing), never past kHoldMaxMs. Times are a monotonic tick in
    // ms; `sinceMs` 0 is no hold.
    constexpr std::uint64_t kHoldMinMs = 1500;
    constexpr std::uint64_t kHoldMaxMs = 15000;

    inline bool HoldActive(std::uint64_t sinceMs, std::uint64_t nowMs, bool lineAlive)
    {
        if (!sinceMs || nowMs < sinceMs)
            return false;
        const std::uint64_t ageMs = nowMs - sinceMs;
        if (ageMs < kHoldMinMs)
            return true;
        return lineAlive && ageMs < kHoldMaxMs;
    }
}
