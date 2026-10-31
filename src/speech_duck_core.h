#pragma once

// The speech duck of the audio engine fed in the replay with the recording's
// state (the input the engine's own driver reads), so the engine starts,
// sets and stops the duck scene itself. Pure part; unit tested.
//
// Why: live, the scripted conversation manager raises the duck level while a
// conversation line is under way, and the ambient speech, weather, ambience,
// shots and horse categories fall 3 to 7 dB in those windows. The mod's lines
// are not the manager's conversations, so in the replay the level stays 0.
//
// The mechanism (a replay records the code scenes' inputs it cannot
// re-derive): the playback writes the duck's inputs right before the driver
// reads them (a detour of the driver that forwards the call): the level and,
// above 0, the four flags. From a clip that recorded the duck scene itself
// (MIXS v3, the pool scan) the level is the recorded apply and each flag is
// its recorded variable above 0; from an older clip the level is 1 while a
// recorded conversation scene is active and the flags are
// SpeechDuckDerivedFlags (default EDE: the measured weatherApply and sfxApply
// need it; EDC, EDD and EDF are not measured).
//
// Evidence: offline dump of build 1491.50 (the signatures below hold the
// bytes); the levels measured in recorded clips.
// Research: docs/research/speech-duck-and-slots.md

#include "engine_sigs.h"
#include "mix_scene_core.h"
#include "mix_scene_pool_core.h" // the game state driver, the duck's start

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace replay::speechduck
{
    using enginesig::kDuckDriver;
    using enginesig::kSoundGroupCaller;

    // ---- engine layout ----

    // The game state driver's call of the duck driver (engine_sigs.h, its
    // prologue up to its first branch). The skip gate (byte: set = the driver
    // skips the duck, another mode) is the global of its cmp [gate],sil.
    inline const sig::Signature kDriverCall{"duck_driver_call", mixscene::pool::kGameStateDriver, sig::Offset{0xE13},
        "E8 ?? ?? ?? ??", 0x2F224F3};
    inline const sig::Signature kDriverCallTarget{"duck_driver_call_target", kDriverCall, sig::Rel32{0x01, 0x05}, "",
        0x2F2D1EC};
    inline const sig::Signature kGateSkip{"duck_gate_skip", kDuckDriver, sig::Rel32{0x20, 0x24}, "", 0x3E04DAB};
    // The driver's reads of its inputs: the level (f32; read, and zeroed every
    // call), the block gate (dword: above 0 = no scene), the duck scene's
    // reference, the mute gate (byte: set = the gated variables are 0), the
    // mode gate (dword: 0 = the gated variables are 0), the four flag bytes
    // EDC..EDF (EDE read twice), the second level (f32) and the second duck's
    // reference.
    inline const sig::Signature kLevelRead{"duck_level_read", kDuckDriver, sig::Offset{0x48}, "F3 0F 10 3D ?? ?? ?? ??",
        0x2F2D234};
    inline const sig::Signature kLevel{"duck_level", kLevelRead, sig::Rel32{0x04, 0x08}, "", 0x5B5BF24};
    inline const sig::Signature kLevelZero{"duck_level_zero", kDuckDriver, sig::Offset{0x269}, "21 35 ?? ?? ?? ??",
        0x2F2D455};
    inline const sig::Signature kLevelZeroed{"duck_level_zeroed", kLevelZero, sig::Rel32{0x02, 0x06}, "", 0x5B5BF24};
    inline const sig::Signature kBlockGateRead{"duck_block_gate", kDuckDriver, sig::Offset{0x5D}, "39 35 ?? ?? ?? ??",
        0x2F2D249};
    inline const sig::Signature kGateBlock{"duck_gate_block", kBlockGateRead, sig::Rel32{0x02, 0x06}, "", 0x5C8EAD4};
    inline const sig::Signature kSceneRefRead{"duck_scene_ref_read", kDuckDriver, sig::Offset{0x69},
        "48 8B 3D ?? ?? ?? ??",
        0x2F2D255};
    inline const sig::Signature kSceneRef{"duck_scene_ref", kSceneRefRead, sig::Rel32{0x03, 0x07}, "", 0x5BA5FE0};
    inline const sig::Signature kMuteGateRead{"duck_mute_gate", kDuckDriver, sig::Offset{0xE3}, "40 38 35 ?? ?? ?? ??",
        0x2F2D2CF};
    inline const sig::Signature kGateMute{"duck_gate_mute", kMuteGateRead, sig::Rel32{0x03, 0x07}, "", 0x5B5BE7B};
    inline const sig::Signature kModeGateRead{"duck_mode_gate", kDuckDriver, sig::Offset{0x108}, "39 35 ?? ?? ?? ??",
        0x2F2D2F4};
    inline const sig::Signature kGateMode{"duck_gate_mode", kModeGateRead, sig::Rel32{0x02, 0x06}, "", 0x5C0FC30};
    inline const sig::Signature kFlagEdeCurveRead{"duck_flag_ede_curve", kDuckDriver, sig::Offset{0x114},
        "40 38 35 ?? ?? ?? ??", 0x2F2D300};
    inline const sig::Signature kFlagEdeCurveByte{"duck_flag_ede_curve_byte", kFlagEdeCurveRead, sig::Rel32{0x03, 0x07},
        "", 0x5B82EDE};
    inline const sig::Signature kFlagEdcRead{"duck_flag_edc", kDuckDriver, sig::Offset{0x12C}, "40 38 35 ?? ?? ?? ??",
        0x2F2D318};
    inline const sig::Signature kFlags{"duck_flags", kFlagEdcRead, sig::Rel32{0x03, 0x07}, "", 0x5B82EDC};
    inline const sig::Signature kFlagEddRead{"duck_flag_edd", kDuckDriver, sig::Offset{0x14E}, "40 38 35 ?? ?? ?? ??",
        0x2F2D33A};
    inline const sig::Signature kFlagEddByte{"duck_flag_edd_byte", kFlagEddRead, sig::Rel32{0x03, 0x07}, "", 0x5B82EDD};
    inline const sig::Signature kFlagEdeRead{"duck_flag_ede", kDuckDriver, sig::Offset{0x168}, "40 38 35 ?? ?? ?? ??",
        0x2F2D354};
    inline const sig::Signature kFlagEdeByte{"duck_flag_ede_byte", kFlagEdeRead, sig::Rel32{0x03, 0x07}, "", 0x5B82EDE};
    inline const sig::Signature kFlagEdfRead{"duck_flag_edf", kDuckDriver, sig::Offset{0x182}, "40 38 35 ?? ?? ?? ??",
        0x2F2D36E};
    inline const sig::Signature kFlagEdfByte{"duck_flag_edf_byte", kFlagEdfRead, sig::Rel32{0x03, 0x07}, "", 0x5B82EDF};
    inline const sig::Signature kLevel2Read{"duck_level2_read", kDuckDriver, sig::Offset{0x26F},
        "F3 0F 10 15 ?? ?? ?? ??",
        0x2F2D45B};
    inline const sig::Signature kLevel2{"duck_level2", kLevel2Read, sig::Rel32{0x04, 0x08}, "", 0x5B5BF48};
    inline const sig::Signature kScene2RefRead{"duck_scene2_ref_read", kDuckDriver, sig::Offset{0x27E},
        "48 8B 0D ?? ?? ?? ??", 0x2F2D46A};
    inline const sig::Signature kScene2Ref{"duck_scene2_ref", kScene2RefRead, sig::Rel32{0x03, 0x07}, "", 0x5BA5FE8};
    // The conversation manager's update sets the level to 1.0; the flags'
    // owner clears the four bytes every update; the second level's update
    // writes it.
    inline const sig::Signature kConversationUpdate{"duck_conversation_update",
        "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 48 8B D8", 0x2FCC0FC};
    inline const sig::Signature kManagerLevelSet{"duck_manager_level_set", kConversationUpdate, sig::Offset{0xDC},
        "C7 05 ?? ?? ?? ?? 00 00 80 3F", 0x2FCC1D8};
    inline const sig::Signature kManagerLevel{"duck_manager_level", kManagerLevelSet, sig::Rel32{0x02, 0x0A}, "",
        0x5B5BF24};
    inline const sig::Signature kFlagsCleared{"duck_flags_cleared", kSoundGroupCaller, sig::Offset{0x234},
        "83 25 ?? ?? ?? ?? 00", 0x2EF2184};
    inline const sig::Signature kFlagsClearedTarget{"duck_flags_cleared_target", kFlagsCleared, sig::Rel32{0x02, 0x07},
        "", 0x5B82EDC};
    inline const sig::Signature kLevel2Update{"duck_level2_update",
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 50 8B A9 80 09 00 00", 0x2FC8864};
    inline const sig::Signature kLevel2Written{"duck_level2_written", kLevel2Update, sig::Offset{0x12B},
        "F3 0F 11 35 ?? ?? ?? ??", 0x2FC898F};
    inline const sig::Signature kLevel2WrittenTarget{"duck_level2_written_target", kLevel2Written,
        sig::Rel32{0x04, 0x08}, "", 0x5B5BF48};

    constexpr std::uint32_t kDuckScene = 0x576520E1u;
    constexpr std::uint32_t kDuck2Scene = 0x0E585610u;
    constexpr std::uint32_t kApply = 0xE865CDE8u;
    constexpr std::uint32_t kVarCurve = 0xCC9C15B6u;   // level * (EDE ? curve : 0)
    constexpr std::uint32_t kVarEdc = 0x015D3903u;     // level * EDC
    constexpr std::uint32_t kVarEdd = 0xE269C300u;     // level * EDD
    constexpr std::uint32_t kVarEde = 0x934A8049u;     // sfxApply = level * EDE
    constexpr std::uint32_t kVarEdf = 0xB580F7ABu;     // level * EDF
    constexpr std::uint32_t kVarWeather = 0x965062BEu; // weatherApply = level * (EDE ? a weather global : 0)
    constexpr std::uint32_t kVarLevel2 = 0xCA9ED31Du;  // the second duck's level
    constexpr std::uint32_t kDuckVars[7] = {kApply, kVarCurve, kVarEdc, kVarEdd, kVarEde, kVarEdf, kVarWeather};

    constexpr std::uint8_t kFlagEdc = 1;
    constexpr std::uint8_t kFlagEdd = 2;
    constexpr std::uint8_t kFlagEde = 4;
    constexpr std::uint8_t kFlagEdf = 8;
    constexpr std::uint8_t kDefaultDerivedFlags = kFlagEde;

    // A site as the log names it: the instruction, what its call or rip
    // operand reaches, and the global that reach must be (`plus` bytes after
    // it): every site of one global agrees, as when they all had its address.
    struct Site
    {
        const char* name;
        const sig::Signature& at;
        const sig::Signature* reach;
        const sig::Signature* same;
        std::uint32_t plus;
    };
    inline const Site kSites[] = {
        {"driver_call", kDriverCall, &kDriverCallTarget, &kDuckDriver, 0},
        {"level_read", kLevelRead, &kLevel, nullptr, 0},
        {"level_zero", kLevelZero, &kLevelZeroed, &kLevel, 0},
        {"block_gate", kBlockGateRead, &kGateBlock, nullptr, 0},
        {"scene_ref", kSceneRefRead, &kSceneRef, nullptr, 0},
        {"mute_gate", kMuteGateRead, &kGateMute, nullptr, 0},
        {"mode_gate", kModeGateRead, &kGateMode, nullptr, 0},
        {"flag_ede_curve", kFlagEdeCurveRead, &kFlagEdeCurveByte, &kFlags, 2},
        {"flag_edc", kFlagEdcRead, &kFlags, nullptr, 0},
        {"flag_edd", kFlagEddRead, &kFlagEddByte, &kFlags, 1},
        {"flag_ede", kFlagEdeRead, &kFlagEdeByte, &kFlags, 2},
        {"flag_edf", kFlagEdfRead, &kFlagEdfByte, &kFlags, 3},
        {"level2_read", kLevel2Read, &kLevel2, nullptr, 0},
        {"scene2_ref", kScene2RefRead, &kScene2Ref, nullptr, 0},
        {"manager_level_set", kManagerLevelSet, &kManagerLevel, &kLevel, 0},
        {"flags_cleared", kFlagsCleared, &kFlagsClearedTarget, &kFlags, 0},
        {"level2_written", kLevel2Written, &kLevel2WrittenTarget, &kLevel2, 0},
    };
    constexpr std::size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

    // ---- the plan ----

    enum class Source : std::uint8_t
    {
        None,     // no MIXS: the engine's own inputs stay
        Derived,  // an older clip: the recorded conversations give the level
        Recorded, // MIXS v3: the recorded duck scenes give the level and the flags
    };
    inline const char* SourceName(Source s)
    {
        switch (s)
        {
        case Source::Derived: return "derived";
        case Source::Recorded: return "recorded";
        default: return "none";
        }
    }

    struct Input
    {
        Source source = Source::None;
        float level = 0.0f;        // the level (kLevel)
        std::uint8_t flags = 0;    // EDC..EDF (bits 0..3), written only when level > 0
        bool level2Known = false;  // the second level (kLevel2) written (recorded source only)
        float level2 = 0.0f;
        std::uint32_t instance = 0; // the recorded instance that gives it (0 none)
    };

    inline bool SameInput(const Input& a, const Input& b)
    {
        return a.source == b.source && a.level == b.level && a.flags == b.flags && a.level2Known == b.level2Known &&
            a.level2 == b.level2;
    }

    inline float VarOr(const std::vector<std::pair<std::uint32_t, float>>& vars, std::uint32_t name, float otherwise)
    {
        for (const auto& v : vars)
            if (v.first == name)
                return v.second;
        return otherwise;
    }

    // The duck's inputs at timeMs from the clip's mix scene track.
    inline Input Plan(const mixscene::Track& track, std::uint32_t timeMs, std::uint8_t derivedFlags)
    {
        Input in;
        if (track.Empty())
            return in;
        if (track.flags & mixscene::kTrackPoolScanned)
        {
            in.source = Source::Recorded;
            in.level2Known = true;
            for (const mixscene::Instance& i : track.instances)
            {
                // Only the two ducks read their variables: VarsAt walks the
                // clip's variables up to timeMs, a pool-scanned clip keeps
                // every open scene active, and this runs every playback tick.
                if ((i.hash != kDuckScene && i.hash != kDuck2Scene) || !i.ActiveAt(timeMs))
                    continue;
                if (i.hash == kDuckScene && in.instance)
                    continue;
                const auto vars = track.VarsAt(i.id, timeMs);
                if (i.hash == kDuckScene)
                {
                    in.instance = i.id;
                    in.level = VarOr(vars, kApply, 1.0f);
                    in.flags = static_cast<std::uint8_t>((VarOr(vars, kVarEdc, 0.0f) > 0.0f ? kFlagEdc : 0) |
                        (VarOr(vars, kVarEdd, 0.0f) > 0.0f ? kFlagEdd : 0) | (VarOr(vars, kVarEde, 0.0f) > 0.0f ? kFlagEde : 0) |
                        (VarOr(vars, kVarEdf, 0.0f) > 0.0f ? kFlagEdf : 0));
                }
                else if (i.hash == kDuck2Scene)
                    in.level2 = VarOr(vars, kVarLevel2, 1.0f);
            }
            if (!(in.level > 0.0f))
            {
                in.level = 0.0f;
                in.flags = 0;
            }
            if (!(in.level2 > 0.0f))
                in.level2 = 0.0f;
            return in;
        }
        in.source = Source::Derived;
        for (const mixscene::Instance& i : track.instances)
            if (i.kind == mixscene::Kind::Conversation && i.ActiveAt(timeMs))
            {
                in.instance = i.id;
                in.level = 1.0f;
                in.flags = static_cast<std::uint8_t>(derivedFlags & 0x0Fu);
                break;
            }
        return in;
    }

    inline std::string FlagsText(std::uint8_t flags)
    {
        char b[8];
        std::snprintf(b, sizeof(b), "0x%x", static_cast<unsigned>(flags & 0x0Fu));
        return b;
    }
}
