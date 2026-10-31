#pragma once

// The mixer's scene pool read directly (every scene the engine has, not only
// the starts the mod saw), the scenes the engine starts straight into the pool
// by game state ("drivers"), and the replay's ownership of the hidden live
// player's state scenes. Pure part (the signatures and the rules); unit tested
// in tests/category_state_tests.cpp.
//
// Why: the driver scenes go straight into the pool start, so their hash is
// never written at +0x328 and no start detour sees them; a list of open scenes
// kept from the starts alone misses the session's permanent scenes when the
// hooks warm up late, and the replay would start second instances of them.
// The player state scenes would follow the hidden live player in the replay.
// Evidence: offline dump (the signatures below) and observed live.
// Research: docs/research/audio-mix-scenes.md

#include "engine_sigs.h"
#include "game_sig_core.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace replay::mixscene::pool
{
    using enginesig::kDuckDriver;
    using enginesig::kMixerStartScene;

    // ---- the encoded pool pointer ----

    // The pool start: its reads of the three cells, the decode, and its store
    // of the scene into the reference.
    inline const sig::Signature kPoolStart{"mix_pool_start",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B E9 49 8B F0 48 8B FA 48 85 D2 0F 84 ?? ?? "
        "?? ??",
        0x2F18E40};
    inline const sig::Signature kPoolValidRead{"mix_pool_valid", kPoolStart, sig::Offset{0x26}, "8A 05 ?? ?? ?? ??",
        0x2F18E66};
    inline const sig::Signature kPoolValidCell{"mix_pool_valid_cell", kPoolValidRead, sig::Rel32{0x02, 0x06}, "",
        0x5CAF468};
    inline const sig::Signature kPoolKeyRead{"mix_pool_key", kPoolStart, sig::Offset{0x34}, "48 8B 0D ?? ?? ?? ??",
        0x2F18E74};
    inline const sig::Signature kPoolKeyCell{"mix_pool_key_cell", kPoolKeyRead, sig::Rel32{0x03, 0x07}, "", 0x5CAF478};
    inline const sig::Signature kPoolValueRead{"mix_pool_value", kPoolStart, sig::Offset{0x3B}, "48 8B 15 ?? ?? ?? ??",
        0x2F18E7B};
    inline const sig::Signature kPoolValueCell{"mix_pool_value_cell", kPoolValueRead, sig::Rel32{0x03, 0x07}, "",
        0x5CAF470};
    inline const sig::Signature kPoolDecode{"mix_pool_decode", kPoolStart, sig::Offset{0x42},
        "48 C1 C9 05 48 C1 C1 20 48 33 D1 8B C1 83 E0 1F 48 C1 C2 20 FF C0 8A C8 8A 05 ?? ?? ?? ?? 48 D3 C2",
        0x2F18E82};
    inline const sig::Signature kPoolNot{"mix_pool_not", kPoolStart, sig::Offset{0x6B}, "48 F7 D2", 0x2F18EAB};
    inline const sig::Signature kSceneReferenceStore{"mix_scene_reference_store", kPoolStart, sig::Offset{0x145},
        "48 89 1E 48 89 B3 10 03 00 00", 0x2F18F85};
    // The pool update's loop over the slots: count, flags, stride, the free
    // bit, base.
    inline const sig::Signature kPoolUpdate{"mix_pool_update",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 80 B9 7C 03 00 00 00",
        0x2F1C5A8};
    inline const sig::Signature kPoolCountRead{"mix_pool_count", kPoolUpdate, sig::Offset{0x340}, "8B 73 18",
        0x2F1C8E8};
    inline const sig::Signature kPoolFlagsRead{"mix_pool_flags", kPoolUpdate, sig::Offset{0x348}, "48 8B 43 10",
        0x2F1C8F0};
    inline const sig::Signature kPoolStrideRead{"mix_pool_stride", kPoolUpdate, sig::Offset{0x357}, "0F AF 43 1C",
        0x2F1C8FF};
    inline const sig::Signature kPoolFreeBit{"mix_pool_free_bit", kPoolUpdate, sig::Offset{0x35B}, "81 E7 80 00 00 00",
        0x2F1C903};
    inline const sig::Signature kPoolBaseRead{"mix_pool_base", kPoolUpdate, sig::Offset{0x36E}, "48 03 53 08",
        0x2F1C916};

    inline std::uint64_t Rol(std::uint64_t v, unsigned n)
    {
        n &= 63u;
        return n ? (v << n) | (v >> (64u - n)) : v;
    }
    inline std::uint64_t Ror(std::uint64_t v, unsigned n) { return Rol(v, (64u - (n & 63u)) & 63u); }

    // As the pool start decodes it (kPoolKeyRead to kPoolNot).
    inline std::uint64_t Decode(std::uint64_t value, std::uint64_t key)
    {
        const std::uint64_t k = Rol(Ror(key, 5), 32);
        std::uint64_t d = value ^ k;
        const unsigned shift = (static_cast<std::uint32_t>(k) & 0x1Fu) + 1u;
        d = Rol(d, 32);
        d = Rol(d, shift);
        return ~d;
    }
    // The inverse (tests build a pool cell with it).
    inline std::uint64_t Encode(std::uint64_t pointer, std::uint64_t key)
    {
        const std::uint64_t k = Rol(Ror(key, 5), 32);
        const unsigned shift = (static_cast<std::uint32_t>(k) & 0x1Fu) + 1u;
        std::uint64_t d = ~pointer;
        d = Ror(d, shift);
        d = Ror(d, 32);
        return d ^ k;
    }

    // The wrapper (the mixer's start by hash, which the theatre hooks) and its
    // store of the hash; the scene's settings set and its constructor.
    inline const sig::Signature kSceneHashStore{"mix_scene_hash_store", kMixerStartScene, sig::Offset{0x6F},
        "89 8A 28 03 00 00", 0x2F19087};
    inline const sig::Signature kSceneSettingsSet{"mix_scene_settings_set",
        "48 89 5C 24 08 57 48 83 EC 20 48 89 91 08 03 00 00", 0x32772A8};
    inline const sig::Signature kSceneSettingsStore{"mix_scene_settings_store", kSceneSettingsSet, sig::Offset{0x0A},
        "48 89 91 08 03 00 00", 0x32772B2};
    inline const sig::Signature kSceneConstruct{"mix_scene_construct", "45 33 C9 48 8D 41 18 4C 89 49 0C 44 89 49 08",
        0x327334C};
    inline const sig::Signature kSceneHashZeroed{"mix_scene_hash_zeroed", kSceneConstruct, sig::Offset{0x80},
        "4C 89 89 28 03 00 00", 0x32733CC};

    // A code site the scan relies on, as the log names it, and for a rip
    // operand the cell it reaches.
    struct CodeSite
    {
        const char* name;
        const sig::Signature& at;
        const sig::Signature* reach;
    };
    inline const CodeSite kSites[] = {
        {"pool_valid", kPoolValidRead, &kPoolValidCell},
        {"pool_key", kPoolKeyRead, &kPoolKeyCell},
        {"pool_value", kPoolValueRead, &kPoolValueCell},
        // ror rcx,5; rol rcx,20h; xor rdx,rcx; mov eax,ecx; and eax,1Fh; rol rdx,20h; inc eax; mov cl,al;
        // mov al,[valid]; rol rdx,cl
        {"pool_decode", kPoolDecode, nullptr},
        {"pool_not", kPoolNot, nullptr},
        {"pool_count", kPoolCountRead, nullptr},
        {"pool_flags", kPoolFlagsRead, nullptr},
        {"pool_stride", kPoolStrideRead, nullptr},
        {"pool_free_bit", kPoolFreeBit, nullptr},
        {"pool_base", kPoolBaseRead, nullptr},
        {"scene_reference_store", kSceneReferenceStore, nullptr},
        {"scene_hash_store", kSceneHashStore, nullptr},
        {"scene_settings_store", kSceneSettingsStore, nullptr},
        {"scene_hash_zeroed", kSceneHashZeroed, nullptr},
    };
    constexpr std::size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

    // Ready to delete, whole up to its ret (93 bytes): the layout of
    // ReadyToDelete.
    inline const sig::Signature kReadyToDelete{"mix_ready_to_delete",
        "48 8D 41 70 45 33 C9 48 8D 91 70 01 00 00 EB 09 4C 39 08 75 48 48 83 C0 10 48 3B C2 75 F2 41 8B D1 48 8D 81 "
        "70 01 00 00 4C 39 08 75 30 FF C2 48 83 C0 10 83 FA 10 72 F0 48 8D 91 70 02 00 00 48 63 89 F0 02 00 00 4C 8D "
        "04 CA EB 09 4C 39 0A 75 0C 48 83 C2 08 49 3B D0 75 F2 B0 01 C3",
        0x3278B0C};

    // ---- the pool and the scene ----
    constexpr std::uint32_t kPoolBase = 0x08;
    constexpr std::uint32_t kPoolFlags = 0x10;
    constexpr std::uint32_t kPoolCount = 0x18;
    constexpr std::uint32_t kPoolStride = 0x1C;
    constexpr std::uint8_t kSlotFree = 0x80;
    constexpr std::uint32_t kMaxSlots = 2048;

    constexpr std::uint32_t kSceneSize = 0x340;
    constexpr std::uint32_t kSceneVars = 0x18;      // 10 pairs {name hash, value}
    constexpr std::uint32_t kSceneVarCount = 0x68;
    constexpr std::uint32_t kScenePatches = 0x70;   // 16 x {patch, group}
    constexpr std::uint32_t kSceneSlots2 = 0x170;   // 16 x 0x10
    constexpr std::uint32_t kSceneList = 0x270;     // qwords, count at +0x2F0
    constexpr std::uint32_t kSceneListCount = 0x2F0;
    constexpr std::uint32_t kSceneSettings = 0x308;
    constexpr std::uint32_t kSceneReference = 0x310;
    constexpr std::uint32_t kSceneOwner = 0x318;
    constexpr std::uint32_t kSceneHash = 0x328;
    constexpr std::uint32_t kApplyVar = 0xE865CDE8u; // joaat("apply")

    inline std::uint64_t Qword(const std::uint8_t* scene, std::uint32_t offset)
    {
        std::uint64_t v = 0;
        std::memcpy(&v, scene + offset, 8);
        return v;
    }
    inline std::uint32_t Dword(const std::uint8_t* scene, std::uint32_t offset)
    {
        std::uint32_t v = 0;
        std::memcpy(&v, scene + offset, 4);
        return v;
    }

    // kReadyToDelete on a copy of the scene (kSceneSize bytes).
    inline bool ReadyToDelete(const std::uint8_t* scene)
    {
        for (std::uint32_t n = 0; n < 16; ++n)
            if (Qword(scene, kScenePatches + n * 0x10))
                return false;
        for (std::uint32_t n = 0; n < 16; ++n)
            if (Qword(scene, kSceneSlots2 + n * 0x10))
                return false;
        const std::int32_t count = static_cast<std::int32_t>(Dword(scene, kSceneListCount));
        for (std::int32_t n = 0; n < count && n < 16; ++n)
            if (Qword(scene, kSceneList + static_cast<std::uint32_t>(n) * 8))
                return false;
        return true;
    }

    inline std::uint32_t Patches(const std::uint8_t* scene)
    {
        std::uint32_t count = 0;
        for (std::uint32_t n = 0; n < 16; ++n)
            count += Qword(scene, kScenePatches + n * 0x10) ? 1u : 0u;
        return count;
    }

    // The value of variable `name` in the scene copy (false when absent).
    inline bool VarOf(const std::uint8_t* scene, std::uint32_t name, float& value)
    {
        const std::int32_t count = static_cast<std::int32_t>(Dword(scene, kSceneVarCount));
        for (std::int32_t n = 0; n < count && n < 10; ++n)
        {
            const std::uint32_t at = kSceneVars + static_cast<std::uint32_t>(n) * 8;
            if (Dword(scene, at) == name)
            {
                std::memcpy(&value, scene + at + 4, 4);
                return true;
            }
        }
        return false;
    }

    // ---- the drivers ----

    enum class Via : std::uint8_t
    {
        Direct,  // straight into the pool start: no hash at +0x328, named by its reference
        Wrapper, // through the wrapper: the start detour sees it (refusable by its return address)
    };

    // The drivers' functions: the duck driver, the game state driver (one
    // call per audio update) and the player state driver.
    inline const sig::Signature kGameStateDriver{"mix_game_state_driver",
        "48 8B C4 55 53 56 57 41 54 41 55 41 56 48 8D A8 F8 FE FF FF", 0x2F216E0};
    inline const sig::Signature kPlayerStateDriver{"mix_player_state_driver",
        "48 89 5C 24 18 55 48 8B EC 48 83 EC 70 8A 05 ?? ?? ?? ??", 0x2F297E4};

    // Per driver: the hash it starts (mov edx,hash or mov dword [rbp+x],hash),
    // the lea r8,[reference] and the reference, the call of the start and its
    // target (the pool start or the wrapper, checked with its bytes).
    inline const sig::Signature kSpeechDuckHash{"mix_speech_duck_hash", kDuckDriver, sig::Offset{0x7F},
        "BA E1 20 65 57", 0x2F2D26B};
    inline const sig::Signature kSpeechDuckLea{"mix_speech_duck_lea", kDuckDriver, sig::Offset{0x95},
        "4C 8D 05 ?? ?? ?? ??", 0x2F2D281};
    inline const sig::Signature kSpeechDuckRef{"mix_speech_duck_ref", kSpeechDuckLea, sig::Rel32{0x03, 0x07}, "",
        0x5BA5FE0};
    inline const sig::Signature kSpeechDuckCall{"mix_speech_duck_call", kDuckDriver, sig::Offset{0xBD},
        "E8 ?? ?? ?? ??", 0x2F2D2A9};
    inline const sig::Signature kSpeechDuckStart{"mix_speech_duck_start", kSpeechDuckCall, sig::Rel32{0x01, 0x05},
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B E9 49 8B F0 48 8B FA 48 85 D2 0F 84 ?? ?? "
        "?? ??",
        0x2F18E40};
    inline const sig::Signature kSpeechDuck2Hash{"mix_speech_duck_2_hash", kDuckDriver, sig::Offset{0x296},
        "BA 10 56 58 0E", 0x2F2D482};
    inline const sig::Signature kSpeechDuck2Lea{"mix_speech_duck_2_lea", kDuckDriver, sig::Offset{0x2AC},
        "4C 8D 05 ?? ?? ?? ??", 0x2F2D498};
    inline const sig::Signature kSpeechDuck2Ref{"mix_speech_duck_2_ref", kSpeechDuck2Lea, sig::Rel32{0x03, 0x07}, "",
        0x5BA5FE8};
    inline const sig::Signature kSpeechDuck2Call{"mix_speech_duck_2_call", kDuckDriver, sig::Offset{0x2D4},
        "E8 ?? ?? ?? ??", 0x2F2D4C0};
    inline const sig::Signature kSpeechDuck2Start{"mix_speech_duck_2_start", kSpeechDuck2Call, sig::Rel32{0x01, 0x05},
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B E9 49 8B F0 48 8B FA 48 85 D2 0F 84 ?? ?? "
        "?? ??",
        0x2F18E40};
    inline const sig::Signature kState3e338f46Hash{"mix_state_3e338f46_hash", kGameStateDriver, sig::Offset{0x9D},
        "BA 46 8F 33 3E", 0x2F2177D};
    inline const sig::Signature kState3e338f46Lea{"mix_state_3e338f46_lea", kGameStateDriver, sig::Offset{0xB9},
        "4C 8D 05 ?? ?? ?? ??", 0x2F21799};
    inline const sig::Signature kState3e338f46Ref{"mix_state_3e338f46_ref", kState3e338f46Lea, sig::Rel32{0x03, 0x07},
        "", 0x5BA5F08};
    inline const sig::Signature kState3e338f46Call{"mix_state_3e338f46_call", kGameStateDriver, sig::Offset{0xE2},
        "E8 ?? ?? ?? ??", 0x2F217C2};
    inline const sig::Signature kState3e338f46Start{"mix_state_3e338f46_start", kState3e338f46Call,
        sig::Rel32{0x01, 0x05},
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B E9 49 8B F0 48 8B FA 48 85 D2 0F 84 ?? ?? "
        "?? ??",
        0x2F18E40};
    inline const sig::Signature kState592b49d2Hash{"mix_state_592b49d2_hash", kGameStateDriver, sig::Offset{0x1F0},
        "BA D2 49 2B 59", 0x2F218D0};
    inline const sig::Signature kState592b49d2Lea{"mix_state_592b49d2_lea", kGameStateDriver, sig::Offset{0x20C},
        "4C 8D 05 ?? ?? ?? ??", 0x2F218EC};
    inline const sig::Signature kState592b49d2Ref{"mix_state_592b49d2_ref", kState592b49d2Lea, sig::Rel32{0x03, 0x07},
        "", 0x5BA5F38};
    inline const sig::Signature kState592b49d2Call{"mix_state_592b49d2_call", kGameStateDriver, sig::Offset{0x235},
        "E8 ?? ?? ?? ??", 0x2F21915};
    inline const sig::Signature kState592b49d2Start{"mix_state_592b49d2_start", kState592b49d2Call,
        sig::Rel32{0x01, 0x05},
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B E9 49 8B F0 48 8B FA 48 85 D2 0F 84 ?? ?? "
        "?? ??",
        0x2F18E40};
    inline const sig::Signature kState5167ffe9Hash{"mix_state_5167ffe9_hash", kGameStateDriver, sig::Offset{0x27D},
        "BA E9 FF 67 51", 0x2F2195D};
    inline const sig::Signature kState5167ffe9Lea{"mix_state_5167ffe9_lea", kGameStateDriver, sig::Offset{0x298},
        "4C 8D 05 ?? ?? ?? ??", 0x2F21978};
    inline const sig::Signature kState5167ffe9Ref{"mix_state_5167ffe9_ref", kState5167ffe9Lea, sig::Rel32{0x03, 0x07},
        "", 0x5BA6078};
    inline const sig::Signature kState5167ffe9Call{"mix_state_5167ffe9_call", kGameStateDriver, sig::Offset{0x2C1},
        "E8 ?? ?? ?? ??", 0x2F219A1};
    inline const sig::Signature kState5167ffe9Start{"mix_state_5167ffe9_start", kState5167ffe9Call,
        sig::Rel32{0x01, 0x05},
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B E9 49 8B F0 48 8B FA 48 85 D2 0F 84 ?? ?? "
        "?? ??",
        0x2F18E40};
    inline const sig::Signature kStateE494db3eHash{"mix_state_e494db3e_hash", kGameStateDriver, sig::Offset{0x30E},
        "BA 3E DB 94 E4", 0x2F219EE};
    inline const sig::Signature kStateE494db3eLea{"mix_state_e494db3e_lea", kGameStateDriver, sig::Offset{0x328},
        "4C 8D 05 ?? ?? ?? ??", 0x2F21A08};
    inline const sig::Signature kStateE494db3eRef{"mix_state_e494db3e_ref", kStateE494db3eLea, sig::Rel32{0x03, 0x07},
        "", 0x5BA6080};
    inline const sig::Signature kStateE494db3eCall{"mix_state_e494db3e_call", kGameStateDriver, sig::Offset{0x34E},
        "E8 ?? ?? ?? ??", 0x2F21A2E};
    inline const sig::Signature kStateE494db3eStart{"mix_state_e494db3e_start", kStateE494db3eCall,
        sig::Rel32{0x01, 0x05},
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B E9 49 8B F0 48 8B FA 48 85 D2 0F 84 ?? ?? "
        "?? ??",
        0x2F18E40};
    inline const sig::Signature kStateF36b4f3aHash{"mix_state_f36b4f3a_hash", kGameStateDriver, sig::Offset{0x3A7},
        "BA 3A 4F 6B F3", 0x2F21A87};
    inline const sig::Signature kStateF36b4f3aLea{"mix_state_f36b4f3a_lea", kGameStateDriver, sig::Offset{0x3C1},
        "4C 8D 05 ?? ?? ?? ??", 0x2F21AA1};
    inline const sig::Signature kStateF36b4f3aRef{"mix_state_f36b4f3a_ref", kStateF36b4f3aLea, sig::Rel32{0x03, 0x07},
        "", 0x5BA6098};
    inline const sig::Signature kStateF36b4f3aCall{"mix_state_f36b4f3a_call", kGameStateDriver, sig::Offset{0x3E7},
        "E8 ?? ?? ?? ??", 0x2F21AC7};
    inline const sig::Signature kStateF36b4f3aStart{"mix_state_f36b4f3a_start", kStateF36b4f3aCall,
        sig::Rel32{0x01, 0x05},
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B E9 49 8B F0 48 8B FA 48 85 D2 0F 84 ?? ?? "
        "?? ??",
        0x2F18E40};
    inline const sig::Signature kStateA496be1fHash{"mix_state_a496be1f_hash", kGameStateDriver, sig::Offset{0x426},
        "BA 1F BE 96 A4", 0x2F21B06};
    inline const sig::Signature kStateA496be1fLea{"mix_state_a496be1f_lea", kGameStateDriver, sig::Offset{0x440},
        "4C 8D 05 ?? ?? ?? ??", 0x2F21B20};
    inline const sig::Signature kStateA496be1fRef{"mix_state_a496be1f_ref", kStateA496be1fLea, sig::Rel32{0x03, 0x07},
        "", 0x5BA60A0};
    inline const sig::Signature kStateA496be1fCall{"mix_state_a496be1f_call", kGameStateDriver, sig::Offset{0x466},
        "E8 ?? ?? ?? ??", 0x2F21B46};
    inline const sig::Signature kStateA496be1fStart{"mix_state_a496be1f_start", kStateA496be1fCall,
        sig::Rel32{0x01, 0x05},
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B E9 49 8B F0 48 8B FA 48 85 D2 0F 84 ?? ?? "
        "?? ??",
        0x2F18E40};
    inline const sig::Signature kState703e9005Hash{"mix_state_703e9005_hash", kGameStateDriver, sig::Offset{0x5A6},
        "BA 05 90 3E 70", 0x2F21C86};
    inline const sig::Signature kState703e9005Lea{"mix_state_703e9005_lea", kGameStateDriver, sig::Offset{0x5C0},
        "4C 8D 05 ?? ?? ?? ??", 0x2F21CA0};
    inline const sig::Signature kState703e9005Ref{"mix_state_703e9005_ref", kState703e9005Lea, sig::Rel32{0x03, 0x07},
        "", 0x5BA5F18};
    inline const sig::Signature kState703e9005Call{"mix_state_703e9005_call", kGameStateDriver, sig::Offset{0x5E6},
        "E8 ?? ?? ?? ??", 0x2F21CC6};
    inline const sig::Signature kState703e9005Start{"mix_state_703e9005_start", kState703e9005Call,
        sig::Rel32{0x01, 0x05},
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B E9 49 8B F0 48 8B FA 48 85 D2 0F 84 ?? ?? "
        "?? ??",
        0x2F18E40};
    inline const sig::Signature kState9d7c75d4Hash{"mix_state_9d7c75d4_hash", kGameStateDriver, sig::Offset{0x61E},
        "BA D4 75 7C 9D", 0x2F21CFE};
    inline const sig::Signature kState9d7c75d4Lea{"mix_state_9d7c75d4_lea", kGameStateDriver, sig::Offset{0x638},
        "4C 8D 05 ?? ?? ?? ??", 0x2F21D18};
    inline const sig::Signature kState9d7c75d4Ref{"mix_state_9d7c75d4_ref", kState9d7c75d4Lea, sig::Rel32{0x03, 0x07},
        "", 0x5BA6090};
    inline const sig::Signature kState9d7c75d4Call{"mix_state_9d7c75d4_call", kGameStateDriver, sig::Offset{0x65E},
        "E8 ?? ?? ?? ??", 0x2F21D3E};
    inline const sig::Signature kState9d7c75d4Start{"mix_state_9d7c75d4_start", kState9d7c75d4Call,
        sig::Rel32{0x01, 0x05},
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B E9 49 8B F0 48 8B FA 48 85 D2 0F 84 ?? ?? "
        "?? ??",
        0x2F18E40};
    inline const sig::Signature kStateAb0410f1Hash{"mix_state_ab0410f1_hash", kGameStateDriver, sig::Offset{0x778},
        "BA F1 10 04 AB", 0x2F21E58};
    inline const sig::Signature kStateAb0410f1Lea{"mix_state_ab0410f1_lea", kGameStateDriver, sig::Offset{0x792},
        "4C 8D 05 ?? ?? ?? ??", 0x2F21E72};
    inline const sig::Signature kStateAb0410f1Ref{"mix_state_ab0410f1_ref", kStateAb0410f1Lea, sig::Rel32{0x03, 0x07},
        "", 0x5BA5F70};
    inline const sig::Signature kStateAb0410f1Call{"mix_state_ab0410f1_call", kGameStateDriver, sig::Offset{0x7B8},
        "E8 ?? ?? ?? ??", 0x2F21E98};
    inline const sig::Signature kStateAb0410f1Start{"mix_state_ab0410f1_start", kStateAb0410f1Call,
        sig::Rel32{0x01, 0x05},
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B E9 49 8B F0 48 8B FA 48 85 D2 0F 84 ?? ?? "
        "?? ??",
        0x2F18E40};
    inline const sig::Signature kPlayerStateHash{"mix_player_state_hash", kPlayerStateDriver, sig::Offset{0x195},
        "C7 45 18 88 C6 BF 24", 0x2F29979};
    inline const sig::Signature kPlayerStateLea{"mix_player_state_lea", kPlayerStateDriver, sig::Offset{0x16C},
        "4C 8D 05 ?? ?? ?? ??", 0x2F29950};
    inline const sig::Signature kPlayerStateRef{"mix_player_state_ref", kPlayerStateLea, sig::Rel32{0x03, 0x07}, "",
        0x5BA6038};
    inline const sig::Signature kPlayerStateCall{"mix_player_state_call", kPlayerStateDriver, sig::Offset{0x19C},
        "E8 ?? ?? ?? ??", 0x2F29980};
    inline const sig::Signature kPlayerStateStart{"mix_player_state_start", kPlayerStateCall, sig::Rel32{0x01, 0x05},
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 41 56 48 83 EC 40 49 8B F0 4C 8B F2 8B 12 48 8B F9 41 B0 03 48 8D "
        "0D ?? ?? ?? ?? 49 8B D9 E8 ?? ?? ?? ?? 4C 8B 94 24 80 00 00 00 4C 8B C6 44 8B 4C 24 70 48 8B D0 4C 89 54 24 30 "
        "48 8B CF 4C 8B 54 24 78",
        0x2F19018};
    inline const sig::Signature kPlayerState2Hash{"mix_player_state_2_hash", kPlayerStateDriver, sig::Offset{0xF8},
        "C7 45 10 98 B9 48 23", 0x2F298DC};
    inline const sig::Signature kPlayerState2Lea{"mix_player_state_2_lea", kPlayerStateDriver, sig::Offset{0xCF},
        "4C 8D 05 ?? ?? ?? ??", 0x2F298B3};
    inline const sig::Signature kPlayerState2Ref{"mix_player_state_2_ref", kPlayerState2Lea, sig::Rel32{0x03, 0x07}, "",
        0x5BA6030};
    inline const sig::Signature kPlayerState2Call{"mix_player_state_2_call", kPlayerStateDriver, sig::Offset{0xFF},
        "E8 ?? ?? ?? ??", 0x2F298E3};
    inline const sig::Signature kPlayerState2Start{"mix_player_state_2_start", kPlayerState2Call,
        sig::Rel32{0x01, 0x05},
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 41 56 48 83 EC 40 49 8B F0 4C 8B F2 8B 12 48 8B F9 41 B0 03 48 8D "
        "0D ?? ?? ?? ?? 49 8B D9 E8 ?? ?? ?? ?? 4C 8B 94 24 80 00 00 00 4C 8B C6 44 8B 4C 24 70 48 8B D0 4C 89 54 24 30 "
        "48 8B CF 4C 8B 54 24 78",
        0x2F19018};
    // What a wrapper driver does when the start gives no scene: it tests the
    // reference and tries again the next frame (the refusal's proof): cmp
    // [reference],rbx; je exit and mov rcx,[reference]; test rcx,rcx; je on.
    inline const sig::Signature kPlayerStateTolerance{"mix_player_state_tolerance", kPlayerStateDriver,
        sig::Offset{0x1AA}, "48 39 1D ?? ?? ?? ?? 74 5E", 0x2F2998E};
    inline const sig::Signature kPlayerStateToleranceRef{"mix_player_state_tolerance_ref", kPlayerStateTolerance,
        sig::Rel32{0x03, 0x07}, "", 0x5BA6038};
    inline const sig::Signature kPlayerState2Tolerance{"mix_player_state_2_tolerance", kPlayerStateDriver,
        sig::Offset{0x10D}, "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 3E", 0x2F298F1};
    inline const sig::Signature kPlayerState2ToleranceRef{"mix_player_state_2_tolerance_ref", kPlayerState2Tolerance,
        sig::Rel32{0x03, 0x07}, "", 0x5BA6030};

    struct Driver
    {
        const char* name;
        std::uint32_t hash;
        Via via;
        const sig::Signature& hashSite;  // the hash's instruction (the immediate is its last 4 bytes)
        const sig::Signature& lea;       // lea r8,[reference]
        const sig::Signature& reference; // reached from the lea
        const sig::Signature& call;      // the call of the start (its return: the start's caller)
        const sig::Signature& start;     // reached from the call
        const sig::Signature* tolerance = nullptr;    // a wrapper driver's test of its reference
        const sig::Signature* toleranceRef = nullptr; // the reference that test reads
    };

    // The game state driver's 0x467F39D6 is left out: its reference is also
    // where the pool start keeps any exclusive scene, so the ref does not
    // name it.
    inline const Driver kDrivers[] = {
        {"speech_duck", 0x576520E1u, Via::Direct, kSpeechDuckHash, kSpeechDuckLea, kSpeechDuckRef, kSpeechDuckCall,
            kSpeechDuckStart},
        {"speech_duck_2", 0x0E585610u, Via::Direct, kSpeechDuck2Hash, kSpeechDuck2Lea, kSpeechDuck2Ref,
            kSpeechDuck2Call, kSpeechDuck2Start},
        {"state_3e338f46", 0x3E338F46u, Via::Direct, kState3e338f46Hash, kState3e338f46Lea, kState3e338f46Ref,
            kState3e338f46Call, kState3e338f46Start},
        {"state_592b49d2", 0x592B49D2u, Via::Direct, kState592b49d2Hash, kState592b49d2Lea, kState592b49d2Ref,
            kState592b49d2Call, kState592b49d2Start},
        {"state_5167ffe9", 0x5167FFE9u, Via::Direct, kState5167ffe9Hash, kState5167ffe9Lea, kState5167ffe9Ref,
            kState5167ffe9Call, kState5167ffe9Start},
        {"state_e494db3e", 0xE494DB3Eu, Via::Direct, kStateE494db3eHash, kStateE494db3eLea, kStateE494db3eRef,
            kStateE494db3eCall, kStateE494db3eStart},
        {"state_f36b4f3a", 0xF36B4F3Au, Via::Direct, kStateF36b4f3aHash, kStateF36b4f3aLea, kStateF36b4f3aRef,
            kStateF36b4f3aCall, kStateF36b4f3aStart},
        {"state_a496be1f", 0xA496BE1Fu, Via::Direct, kStateA496be1fHash, kStateA496be1fLea, kStateA496be1fRef,
            kStateA496be1fCall, kStateA496be1fStart},
        {"state_703e9005", 0x703E9005u, Via::Direct, kState703e9005Hash, kState703e9005Lea, kState703e9005Ref,
            kState703e9005Call, kState703e9005Start},
        {"state_9d7c75d4", 0x9D7C75D4u, Via::Direct, kState9d7c75d4Hash, kState9d7c75d4Lea, kState9d7c75d4Ref,
            kState9d7c75d4Call, kState9d7c75d4Start},
        {"state_ab0410f1", 0xAB0410F1u, Via::Direct, kStateAb0410f1Hash, kStateAb0410f1Lea, kStateAb0410f1Ref,
            kStateAb0410f1Call, kStateAb0410f1Start},
        {"player_state", 0x24BFC688u, Via::Wrapper, kPlayerStateHash, kPlayerStateLea, kPlayerStateRef,
            kPlayerStateCall, kPlayerStateStart, &kPlayerStateTolerance, &kPlayerStateToleranceRef},
        {"player_state_2", 0x2348B998u, Via::Wrapper, kPlayerState2Hash, kPlayerState2Lea, kPlayerState2Ref,
            kPlayerState2Call, kPlayerState2Start, &kPlayerState2Tolerance, &kPlayerState2ToleranceRef},
    };
    constexpr std::size_t kDriverCount = sizeof(kDrivers) / sizeof(kDrivers[0]);

    // The immediate of a driver's hash instruction (0 when its pattern does
    // not hold one).
    inline std::uint32_t HashImmediate(const Driver& d)
    {
        sig::Pattern p;
        if (!sig::Parse(d.hashSite.pattern, p) || p.size < 5)
            return 0;
        std::uint32_t v = 0;
        for (std::size_t n = 0; n < 4; ++n)
        {
            if (p.wild[p.size - 4 + n])
                return 0;
            v |= static_cast<std::uint32_t>(p.bytes[p.size - 4 + n]) << (8 * n);
        }
        return v;
    }

    // The table agrees with its signatures (the tests and the runtime check
    // before the signatures are looked up): the hash in its instruction, the
    // reference reached from the lea, the start from the call and checked
    // with the bytes of the pool start or the wrapper, a tolerance's
    // reference reached from it.
    inline bool DriverConsistent(const Driver& d)
    {
        const sig::Signature& start = d.via == Via::Direct ? kPoolStart : kMixerStartScene;
        return HashImmediate(d) == d.hash && d.reference.parent == &d.lea && d.start.parent == &d.call &&
            std::strcmp(d.start.pattern, start.pattern) == 0 &&
            (!d.tolerance || (d.toleranceRef && d.toleranceRef->parent == d.tolerance));
    }

    inline std::size_t DriverIndex(const Driver& d) { return static_cast<std::size_t>(&d - kDrivers); }

    inline const Driver* DriverByHash(std::uint32_t hash)
    {
        for (const Driver& d : kDrivers)
            if (d.hash == hash)
                return &d;
        return nullptr;
    }
    // The driver whose reference holds the scene: references[n] is where
    // kDrivers[n]'s reference is in the game (0: not found, never matched).
    inline const Driver* DriverByReference(std::uint64_t reference, const std::uintptr_t* references)
    {
        if (!reference || !references)
            return nullptr;
        for (std::size_t n = 0; n < kDriverCount; ++n)
            if (references[n] == reference)
                return &kDrivers[n];
        return nullptr;
    }
    // A wrapper driver whose null reference is proven harmless (the replay
    // may refuse its start while it owns the scene; its tolerance must read
    // the same reference in the game).
    inline bool Refusable(const Driver& d) { return d.via == Via::Wrapper && d.tolerance && d.toleranceRef; }

    // ---- a scan ----

    enum class Name : std::uint8_t
    {
        None,    // no hash at +0x328 and no driver reference
        Wrapper, // the hash at +0x328
        Direct,  // a driver's reference
    };
    inline const char* NameText(Name n)
    {
        switch (n)
        {
        case Name::Wrapper: return "wrapper";
        case Name::Direct: return "direct";
        default: return "none";
        }
    }

    struct PoolScene
    {
        std::uint32_t slot = 0;
        std::uint64_t scene = 0;
        std::uint64_t settings = 0;
        std::uint64_t reference = 0; // where the scene is kept (+0x310)
        std::uint64_t owner = 0;
        std::uint32_t hash = 0;      // +0x328, else the driver's
        Name name = Name::None;
        const Driver* driver = nullptr;
        std::uint32_t patches = 0;
        bool hasApply = false;
        float apply = 0.0f;
        bool replayOwned = false;    // a reference of the replay's own
    };

    // Names a scene copy: +0x328 when set, else the driver whose reference
    // holds it (DriverByReference, null when none).
    inline void NameScene(PoolScene& s, std::uint32_t hashAt328, const Driver* driver)
    {
        s.hash = hashAt328;
        s.name = hashAt328 ? Name::Wrapper : Name::None;
        s.driver = driver;
        if (!hashAt328 && s.driver)
        {
            s.hash = s.driver->hash;
            s.name = Name::Direct;
        }
    }

    // The hashes of the scenes the engine has (not the replay's own).
    inline std::vector<std::uint32_t> EngineOpen(const std::vector<PoolScene>& scenes)
    {
        std::vector<std::uint32_t> out;
        for (const PoolScene& s : scenes)
            if (!s.replayOwned && s.hash)
                out.push_back(s.hash);
        return out;
    }

    // The same, leaving out too the scenes the replay is running now, by pointer.
    // A script scene the replay starts with START_AUDIO_SCENE (the script start
    // into the wrapper) keeps the reference of the script thread's slot, not one
    // of the replay's, so replayOwned does not see it: counted as the engine's,
    // the next tick would stop it as "engine_owns" and start it again until
    // kMaxAttempts, then lose it.
    inline std::vector<std::uint32_t> EngineOpen(const std::vector<PoolScene>& scenes, const std::vector<std::uint64_t>& running)
    {
        std::vector<std::uint32_t> out;
        for (const PoolScene& s : scenes)
            if (!s.replayOwned && s.hash && std::find(running.begin(), running.end(), s.scene) == running.end())
                out.push_back(s.hash);
        return out;
    }

    // How many alive scenes carry each hash (duplicates of one hash).
    inline std::size_t Duplicates(const std::vector<PoolScene>& scenes, std::uint32_t hash)
    {
        std::size_t n = 0;
        for (const PoolScene& s : scenes)
            if (s.hash == hash)
                ++n;
        return n;
    }
}
