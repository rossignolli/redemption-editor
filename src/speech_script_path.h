#pragma once

// The scripted (conversation) lines asked again on their own path.
//
// A scripted line (a campfire story, a mission conversation) is recorded with
// source 2, pD4 0xF and line bits 0x02480000; asked as an ambient line it
// comes back with source 4, pD4 0 and bits 0x02688080, the ambient sound and
// 3 to 4 dB lower at the same distance. Engine facts (build 1491.50):
//   - the slot allocator by source 0x2FBCE38 switches on line +0x218: source
//     2 goes to 0x2FBD638, which takes the first free one of the speech
//     manager's 24 script slots (stride 0x28 at 0x5BB9750; 0x2C32CE0 free,
//     slot vtable +0x20) and loads the line's own wave in it (0x2C2D788):
//     nothing to preload, no bank, independent of the ambient slots the
//     replay manages (speech_slots.h);
//   - pD4 (params +0xD4 -> line +0x1F8) is a 4 bit mask the state 3 rules
//     (0x2F975A4, at 0x2F97897..0x2F978E0) match against the other lines'
//     kind before refusing an overlap; 0xF lets the line coexist with all;
//   - line +0x2BE bit 5 comes from params byte +0x19E (line init 0x2FABBAC,
//     0x2FAC063); 0x2FCC224, called by the line's per-frame update
//     (0x2FC9F47), sees it on a d8 == 2 line of type 1 or 2 and every frame
//     raises the global speech ducking flags (0x5B82EDE/0x5B82EDF) and level
//     (0x5B5BF24) that eight audio engine functions read (0x2EE98BC to
//     0x2F2D1EC; what each one ducks is not identified). The recorded
//     scripted lines had it clear, the replayed ones set (the params
//     default), so a replayed scripted line raised a duck the live one did
//     not;
//   - line +0x2BD bit 7 (params +0x130 != 0) is read only behind the network
//     game flag 0x5298473 (0x2FA1223, 0x2FB182B): inert in story mode, not
//     asked again.
// So a line recorded with source 2 is asked with source 2, the recorded pD4
// and the recorded bit 5 of +0x2BE, and skips the ambient slot gate.
//
// The player's scripted line: live, the player's scripted lines have the
// engine distance d_lst = 1.0 whatever the camera distance (measured 2 to
// 10.5 m), the NPCs' d_lst ~ d_cam - 1; in the replay the hidden player says them (SpeechPlayerOnPlayer) at the
// body, 4 to 9 m from the camera. With SpeechPlayerScriptNear the puppet
// says them, and while the replay renders the recorded game camera (the
// rendered camera within kNearGameCameraM of it) with the frontend volume
// type 2 (0x2F9B800: no position, the volume on component [0]), which is
// what "at 1 m from the listener" sounds like; farther (a free camera) the
// puppet says it positioned at itself. That the live 1.0 m is a listener
// rule of the player's speech is a hypothesis (the d_lst reading itself is
// one, mix_probe.h).
//
// Pure: unit tested in ReplayCoreTests.

#include "speech_request.h"

#include <cstdint>

namespace replay::speech::scriptpath
{
    constexpr std::int32_t kSourceScript = 2;
    constexpr std::uint16_t kParamsPD4 = 0xD4;
    constexpr std::uint32_t kPD4Limit = 0x10;     // a 4 bit mask
    constexpr std::uint16_t kParamsDuckByte = 0x19E;
    // The line bits field (speech_request.h, u32 from +0x2BC): byte 2 is
    // +0x2BE, its bit 5 the duck trigger.
    constexpr std::uint32_t kLineDuckBit = 1u << (16 + 5);
    constexpr std::uint16_t kParamsVolumeType = 0xDC;
    constexpr std::uint32_t kVolumeTypeFrontend = 2;
    // The rendered camera counts as the recorded game camera this close.
    constexpr float kNearGameCameraM = 1.5f;

    inline bool IsScriptLine(const request::Fields& recorded)
    {
        return recorded.read && recorded.value[request::Source] == static_cast<std::uint32_t>(kSourceScript);
    }

    // The source a line is asked with on the script path, 0 when it is not
    // (key off, an animal, not recorded with source 2, an old clip).
    inline std::int32_t Source(bool on, bool animal, const request::Fields& recorded)
    {
        return on && !animal && IsScriptLine(recorded) ? kSourceScript : 0;
    }

    // Adds the script path's request writes (recorded pD4 and duck byte).
    // False when the line does not go that way.
    inline bool Apply(bool on, bool animal, const request::Fields& recorded, request::Ask& ask)
    {
        if (!Source(on, animal, recorded))
            return false;
        if (recorded.value[request::PD4] < kPD4Limit)
            ask.Set(kParamsPD4, recorded.value[request::PD4]);
        ask.SetByte(kParamsDuckByte, (recorded.value[request::LineBits] & kLineDuckBit) ? 1 : 0);
        return true;
    }

    // ---- who says the recorded player's scripted line -------------------

    enum class PlayerNear : std::uint8_t
    {
        Off,        // SpeechPlayerScriptNear=0: the old route (speech_player_route.h)
        NotScript,  // another speaker, or not a scripted line
        Positioned, // the puppet says it, positioned (camera away from the recorded game camera)
        Frontend    // the puppet says it, frontend volume type (recorded game camera)
    };

    inline const char* PlayerNearName(PlayerNear choice)
    {
        switch (choice)
        {
        case PlayerNear::Off: return "off";
        case PlayerNear::NotScript: return "not_script";
        case PlayerNear::Positioned: return "positioned";
        case PlayerNear::Frontend: return "frontend";
        }
        return "unknown";
    }

    // `cameraToGameM`: the rendered camera's distance to the recorded game
    // camera at the playhead (negative: unknown, no GCAM samples).
    inline PlayerNear ChoosePlayerNear(bool on, bool recordedPlayer, bool scriptLine, float cameraToGameM)
    {
        if (!on)
            return PlayerNear::Off;
        if (!recordedPlayer || !scriptLine)
            return PlayerNear::NotScript;
        if (cameraToGameM >= 0.0f && cameraToGameM <= kNearGameCameraM)
            return PlayerNear::Frontend;
        return PlayerNear::Positioned;
    }

    // The puppet (not the hidden player) says the line.
    inline bool PuppetSays(PlayerNear choice)
    {
        return choice == PlayerNear::Positioned || choice == PlayerNear::Frontend;
    }

    inline void ApplyPlayerNear(PlayerNear choice, request::Ask& ask)
    {
        if (choice == PlayerNear::Frontend)
            ask.Set(kParamsVolumeType, kVolumeTypeFrontend);
    }
}
