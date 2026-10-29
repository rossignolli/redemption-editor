#pragma once

// The speech duck of the audio engine fed in the replay with the recording's
// state (the input the engine's own driver reads), so the engine starts,
// sets and stops the duck scene itself. Pure part; unit tested.
//
// Engine facts (build 1491.50):
//   - the driver 0x2F2D1EC (no arguments; called once per audio update by
//     0x2F216E0 at 0x2F224F3, which 0x2F1CE4C calls) reads the duck level
//     [0x5B5BF24] (0x2F2D234): above 0 ([0x330F380]) and with [0x5C8EAD4] not
//     above 0 it starts the scene 0x576520E1 into its reference 0x5BA5FE0
//     (0x2F2D2A9, straight into the pool start 0x2F18E40) and sets seven
//     variables on it: apply 0xE865CDE8 = level; 0xCC9C15B6 = level * (EDE ?
//     curve 0x4D73F743 : 0); 0x015D3903 = level * EDC; 0xE269C300 = level *
//     EDD; sfxApply 0x934A8049 = level * EDE; 0xB580F7AB = level * EDF;
//     weatherApply 0x965062BE = level * (EDE ? [0x3E04E00] : 0), with EDC..EDF
//     the four bytes at 0x5B82EDC (0x2F2D300..0x2F2D395) and only when
//     [0x5B5BE7B] is 0 and [0x5C0FC30] is not (0x2F2D2CF, 0x2F2D2F4); at
//     level 0 it stops the scene (0x2F2D449); it zeroes the level every call
//     (0x2F2D455). A second part does the same for 0x0E585610 (reference
//     0x5BA5FE8, variable 0xCA9ED31D) from the level [0x5B5BF48] (0x2F2D45B),
//     which 0x2FC8864 writes every update (0x2FC898F);
//   - the level is set to 1.0 by the scripted conversation manager
//     (0x2FC4F58 -> 0x2FCC0FC, at 0x2FCC1D8) when one of its conversations
//     has a line under way (0x2FBEE64 -> 0x2FBF194: state 3, past the line's
//     start), and the byte i of 0x5B82EDC is set when a playing
//     conversation's current line has its byte +0x20+i (line +0x100,
//     0x2FBF21C -> 0x2FA8744); the four bytes are cleared every update
//     (0x2EF2184).
// Live, the scene is active with apply 1.0, weatherApply 1.0 and sfxApply
// 1.0 (so EDE was set) exactly in the recorded conversations, and never in
// the replay: the mod's lines are not the manager's conversations, so the
// level stays 0. The ambient speech, weather, ambience, shots and horse
// categories fall 3 to 7 dB in those windows.
//
// The mechanism (a replay records the code scenes' inputs it cannot
// re-derive): the playback writes the duck's inputs right before the driver
// reads them (a detour of 0x2F2D1EC that forwards the call): the level and,
// above 0, the four flags. From a clip that recorded the duck scene itself
// (MIXS v3, the pool scan) the level is the recorded apply and each flag is
// its recorded variable above 0; from an older clip the level is 1 while a
// recorded conversation scene is active and the flags are
// SpeechDuckDerivedFlags (default EDE: the measured weatherApply and sfxApply
// need it; EDC, EDD and EDF are not measured).

#include "mix_scene_core.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace replay::speechduck
{
    // ---- engine layout (build 1491.50) ----
    constexpr std::uint32_t kDriverRva = 0x2F2D1EC;
    constexpr std::uint32_t kLevelRva = 0x5B5BF24;   // f32
    constexpr std::uint32_t kLevel2Rva = 0x5B5BF48;  // f32
    constexpr std::uint32_t kFlagsRva = 0x5B82EDC;   // 4 bytes EDC..EDF
    constexpr std::uint32_t kSceneRefRva = 0x5BA5FE0;
    constexpr std::uint32_t kScene2RefRva = 0x5BA5FE8;
    constexpr std::uint32_t kGateMuteRva = 0x5B5BE7B;   // byte: set = the gated variables are 0
    constexpr std::uint32_t kGateModeRva = 0x5C0FC30;   // dword: 0 = the gated variables are 0
    constexpr std::uint32_t kGateBlockRva = 0x5C8EAD4;  // dword: above 0 = no scene
    constexpr std::uint32_t kGateSkipRva = 0x3E04DAB;   // byte: set = the driver skips the duck (another mode)

    constexpr std::uint32_t kDuckScene = 0x576520E1u;
    constexpr std::uint32_t kDuck2Scene = 0x0E585610u;
    constexpr std::uint32_t kApply = 0xE865CDE8u;
    constexpr std::uint32_t kVarCurve = 0xCC9C15B6u;   // level * (EDE ? curve : 0)
    constexpr std::uint32_t kVarEdc = 0x015D3903u;     // level * EDC
    constexpr std::uint32_t kVarEdd = 0xE269C300u;     // level * EDD
    constexpr std::uint32_t kVarEde = 0x934A8049u;     // sfxApply = level * EDE
    constexpr std::uint32_t kVarEdf = 0xB580F7ABu;     // level * EDF
    constexpr std::uint32_t kVarWeather = 0x965062BEu; // weatherApply = level * (EDE ? [0x3E04E00] : 0)
    constexpr std::uint32_t kVarLevel2 = 0xCA9ED31Du;  // the second duck's level
    constexpr std::uint32_t kDuckVars[7] = {kApply, kVarCurve, kVarEdc, kVarEdd, kVarEde, kVarEdf, kVarWeather};

    constexpr std::uint8_t kFlagEdc = 1;
    constexpr std::uint8_t kFlagEdd = 2;
    constexpr std::uint8_t kFlagEde = 4;
    constexpr std::uint8_t kFlagEdf = 8;
    constexpr std::uint8_t kDefaultDerivedFlags = kFlagEde;

    // The driver's prologue up to its first branch (72 bytes): mov rax,rsp;
    // two saves; push rbp; lea rbp,[rax-5Fh]; sub rsp,0D0h; movaps; xor
    // esi,esi; cmp [0x3E04DAB],sil; five movaps; jne.
    constexpr std::uint8_t kDriverSignature[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x70, 0x08, 0x48, 0x89, 0x78, 0x10, 0x55,
        0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x78, 0xE8, 0x33, 0xF6, 0x40, 0x38,
        0x35, 0x9B, 0x7B, 0xED, 0x00, 0x44, 0x0F, 0x29, 0x40, 0xD8, 0x44, 0x0F, 0x29, 0x48, 0xC8, 0x44, 0x0F, 0x29, 0x50,
        0xB8, 0x44, 0x0F, 0x29, 0x58, 0xA8, 0x44, 0x0F, 0x29, 0x60, 0x98, 0x44, 0x0F, 0x29, 0x68, 0x88, 0x0F, 0x85, 0x27,
        0x02, 0x00, 0x00};

    struct Site
    {
        const char* name;
        std::uint32_t rva;
        std::uint8_t size;
        std::uint8_t bytes[10];
        std::int8_t ripAt;
        std::uint8_t insnEnd;
        std::uint32_t target;
    };
    constexpr Site kSites[] = {
        {"driver_call", 0x2F224F3, 5, {0xE8, 0xF4, 0xAC, 0x00, 0x00}, 1, 5, kDriverRva},
        {"level_read", 0x2F2D234, 8, {0xF3, 0x0F, 0x10, 0x3D, 0xE8, 0xEC, 0xC2, 0x02}, 4, 8, kLevelRva},
        {"level_zero", 0x2F2D455, 6, {0x21, 0x35, 0xC9, 0xEA, 0xC2, 0x02}, 2, 6, kLevelRva},
        {"block_gate", 0x2F2D249, 6, {0x39, 0x35, 0x85, 0x18, 0xD6, 0x02}, 2, 6, kGateBlockRva},
        {"scene_ref", 0x2F2D255, 7, {0x48, 0x8B, 0x3D, 0x84, 0x8D, 0xC7, 0x02}, 3, 7, kSceneRefRva},
        {"mute_gate", 0x2F2D2CF, 7, {0x40, 0x38, 0x35, 0xA5, 0xEB, 0xC2, 0x02}, 3, 7, kGateMuteRva},
        {"mode_gate", 0x2F2D2F4, 6, {0x39, 0x35, 0x36, 0x29, 0xCE, 0x02}, 2, 6, kGateModeRva},
        {"flag_ede_curve", 0x2F2D300, 7, {0x40, 0x38, 0x35, 0xD7, 0x5B, 0xC5, 0x02}, 3, 7, kFlagsRva + 2},
        {"flag_edc", 0x2F2D318, 7, {0x40, 0x38, 0x35, 0xBD, 0x5B, 0xC5, 0x02}, 3, 7, kFlagsRva + 0},
        {"flag_edd", 0x2F2D33A, 7, {0x40, 0x38, 0x35, 0x9C, 0x5B, 0xC5, 0x02}, 3, 7, kFlagsRva + 1},
        {"flag_ede", 0x2F2D354, 7, {0x40, 0x38, 0x35, 0x83, 0x5B, 0xC5, 0x02}, 3, 7, kFlagsRva + 2},
        {"flag_edf", 0x2F2D36E, 7, {0x40, 0x38, 0x35, 0x6A, 0x5B, 0xC5, 0x02}, 3, 7, kFlagsRva + 3},
        {"level2_read", 0x2F2D45B, 8, {0xF3, 0x0F, 0x10, 0x15, 0xE5, 0xEA, 0xC2, 0x02}, 4, 8, kLevel2Rva},
        {"scene2_ref", 0x2F2D46A, 7, {0x48, 0x8B, 0x0D, 0x77, 0x8B, 0xC7, 0x02}, 3, 7, kScene2RefRva},
        {"manager_level_set", 0x2FCC1D8, 10, {0xC7, 0x05, 0x42, 0xFD, 0xB8, 0x02, 0x00, 0x00, 0x80, 0x3F}, 2, 10, kLevelRva},
        {"flags_cleared", 0x2EF2184, 7, {0x83, 0x25, 0x51, 0x0D, 0xC9, 0x02, 0x00}, 2, 7, kFlagsRva},
        {"level2_written", 0x2FC898F, 8, {0xF3, 0x0F, 0x11, 0x35, 0xB1, 0x35, 0xB9, 0x02}, 4, 8, kLevel2Rva},
    };
    constexpr std::size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

    inline std::uint32_t SiteTarget(const Site& site)
    {
        if (site.ripAt < 0)
            return 0;
        const std::uint8_t* d = site.bytes + site.ripAt;
        const std::int32_t disp = static_cast<std::int32_t>(static_cast<std::uint32_t>(d[0]) |
            (static_cast<std::uint32_t>(d[1]) << 8) | (static_cast<std::uint32_t>(d[2]) << 16) |
            (static_cast<std::uint32_t>(d[3]) << 24));
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(site.rva) + site.insnEnd + disp);
    }

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
        float level = 0.0f;        // [0x5B5BF24]
        std::uint8_t flags = 0;    // EDC..EDF (bits 0..3), written only when level > 0
        bool level2Known = false;  // [0x5B5BF48] written (recorded source only)
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
