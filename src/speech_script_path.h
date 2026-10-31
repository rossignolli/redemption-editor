#pragma once

// The scripted (conversation) lines' request fields the replay reads: the
// recorded source 2 (the engine's script slots), the pD4 overlap mask and the
// duck trigger bit (speech_conversation.h writes the duck byte).
//
// The script path (a source 2 line asked again with source 2, its pD4 and duck
// byte, off the ambient slot gate) and the player's scripted lines said by his
// puppet were experiments (SpeechScriptPath, SpeechPlayerScriptNear) that left
// with their keys; the replay asks every human line as before.
//
// Evidence: offline dump of build 1491.50.
// Research: docs/research/speech-line-request.md
//
// Pure: unit tested in ReplayCoreTests.

#include "speech_request.h"

#include <cstdint>

namespace replay::speech::scriptpath
{
    constexpr std::int32_t kSourceScript = 2;
    constexpr std::uint16_t kParamsDuckByte = 0x19E;
    // The line bits field (speech_request.h, u32 from +0x2BC): byte 2 is
    // +0x2BE, its bit 5 the duck trigger.
    constexpr std::uint32_t kLineDuckBit = 1u << (16 + 5);
}
