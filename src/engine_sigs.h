#pragma once

// The engine code more than one module imitates, found by signature
// (game_sig_core.h). The bytes of these functions hold the layout the
// modules read (the offsets, the masks, the row sizes), so a build that moved
// any of them does not match and the modules that need it stay off.
//
// Pure: the declarations only (the tests build them too); game_sig.h
// resolves them in the game.

#include "game_sig_core.h"

#include <cstdint>

namespace replay::enginesig
{
    // The two globals of an entity extension pool: the base index (u32) and
    // the table (u64), where the game has them; 0 when not found.
    struct Pool
    {
        std::uintptr_t baseIndex = 0;
        std::uintptr_t table = 0;

        bool Found() const { return baseIndex && table; }
    };

    // The creature of a ped, the first step of _GET_CHAR_EXPRESSION: its
    // slot u32 [ped+0x9C] & 0x1FFFF minus the base index, an entry of 16 bytes
    // in the table, & ~1, then the creature at +0x28.
    inline const sig::Signature kCreatureLookup{"creature_lookup",
        "8B 89 9C 00 00 00 48 8B 05 ?? ?? ?? ?? 81 E1 FF FF 01 00 2B 0D ?? ?? ?? ?? 48 03 C9 48 8B 14 C8 48 8B CA 48 "
        "83 E1 FE 48 F7 DA 48 1B C0 48 23 C1 74 05 48 8B 40 28 C3 33 C0 C3",
        0x2BDDE1C};
    inline const sig::Signature kCreatureTable{"creature_table", kCreatureLookup, sig::Rel32{0x09, 0x0D}, "", 0x3CD4388};
    inline const sig::Signature kCreatureBaseIndex{"creature_base_index", kCreatureLookup, sig::Rel32{0x15, 0x19}, "",
        0x3CD4380};

    // The graphics component of a ped, as the pre-render builder reads it
    // before it asks the visibility test about it: entity type at +0x30, slot
    // u32 [ped+0x9C] & 0x1FFFF minus the base index, a row of 0x148 bytes in
    // the table, the component at +0x98, & ~1. The call is the visibility
    // test (anim_visible.h).
    inline const sig::Signature kComponentRead{"graphics_component_read",
        "44 38 72 30 75 43 8B 82 9C 00 00 00 41 23 C7 2B 05 ?? ?? ?? ?? 48 69 C8 48 01 00 00 48 8B 05 ?? ?? ?? ?? 48 "
        "8B 94 01 98 00 00 00 48 8B C2 48 83 E0 FE 48 F7 DA 48 1B C9 48 23 C8 74 0C E8 ?? ?? ?? ?? 4C 8B 05 ?? ?? ?? "
        "?? 41 03 DC",
        0x56FA95};
    inline const sig::Signature kComponentBaseIndex{"component_base_index", kComponentRead, sig::Rel32{0x11, 0x15}, "",
        0x39ECE40};
    inline const sig::Signature kComponentTable{"component_table", kComponentRead, sig::Rel32{0x1F, 0x23}, "", 0x39ECE48};

    // ---- functions and globals more than one module names ----------------

    // The gore mask getter (ped) -> its entry: the u16 count and the table it
    // walks (entries of 0x20, the ped at +0x00).
    inline const sig::Signature kGoreMaskGetter{"gore_mask_getter",
        "33 C0 48 85 C9 74 2C 0F B7 15 ?? ?? ?? ?? 48 85 D2 74 20 4C 8B 15 ?? ?? ?? ?? 44 8B C8 4D 8B C2 49 39 08 74 "
        "11 FF C0 49 FF C1 49 83 C0 20 4C 3B CA 7C ED 33 C0 C3 8B C0 48 C1 E0 05 49 03 C2 C3",
        0xB4498C};
    inline const sig::Signature kGoreCount{"gore_count", kGoreMaskGetter, sig::Rel32{0x0A, 0x0E}, "", 0x497CB88}; // u16
    inline const sig::Signature kGoreTable{"gore_table", kGoreMaskGetter, sig::Rel32{0x16, 0x1A}, "", 0x497CB80}; // u64

    // The fragInst constructor stores the fragment instance's vtable.
    inline const sig::Signature kFragInstCtor{"frag_inst_ctor",
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 65 48 8B 14 25 58 00 00 00 48 8D 05 ?? ?? "
        "?? ?? 48 89 03",
        0x6A96E0};
    inline const sig::Signature kFragInstVtable{"frag_inst_vtable", kFragInstCtor, sig::Rel32{0x23, 0x27}, "",
        0x33CBDB8};

    // The ped audio holder: its constructor writes the base vtable; the two
    // ped factories (mov edx, size of 0x4A70 and 0x4CB0) build the two kinds of
    // holder at +0x250 of their peds, each constructor calling the base first
    // and writing its own vtable. Slot +0x68 is the sound query resolver.
    inline const sig::Signature kAudioHolderCtor{"ped_audio_holder_ctor",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B FA 48 8B F1 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? "
        "?? 48 8B D6 48 8D 8E C8 01 00 00 48 89 06",
        0x3178860};
    inline const sig::Signature kAudioHolderVtable{"ped_audio_holder_vtable", kAudioHolderCtor, sig::Rel32{0x22, 0x26},
        "", 0x3839538};
    inline const sig::Signature kAudioHolderFactoryA{"ped_audio_holder_factory_a",
        "BA 70 4A 00 00 49 8B C8 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 74 26 48 8D 98 50 02 00 00 48 8B C8 48 8B D3 E8 ?? ?? "
        "?? ?? 48 8D 05 ?? ?? ?? ?? 48 8B CB 48 89 07 E8 ?? ?? ?? ??",
        0x89DB79};
    inline const sig::Signature kAudioHolderCtorA{"ped_audio_holder_ctor_a", kAudioHolderFactoryA,
        sig::Rel32{0x35, 0x39},
        "48 89 5C 24 08 57 48 83 EC 20 48 8D 99 70 46 00 00 48 8B F9 48 8B D3 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 8B "
        "D7 48 8B CB 48 89 07 E8 ?? ?? ?? ??",
        0x31786FC};
    inline const sig::Signature kAudioHolderVtableA{"ped_audio_holder_vtable_a", kAudioHolderCtorA,
        sig::Rel32{0x1F, 0x23}, "", 0x3839630};
    inline const sig::Signature kAudioHolderFactoryB{"ped_audio_holder_factory_b",
        "BA B0 4C 00 00 49 8B C8 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 74 26 48 8D 98 50 02 00 00 48 8B C8 48 8B D3 E8 ?? ?? "
        "?? ?? 48 8D 05 ?? ?? ?? ?? 48 8B CB 48 89 07 E8 ?? ?? ?? ??",
        0x89E5A7};
    inline const sig::Signature kAudioHolderCtorB{"ped_audio_holder_ctor_b", kAudioHolderFactoryB,
        sig::Rel32{0x35, 0x39},
        "48 89 5C 24 08 57 48 83 EC 20 48 8D 99 70 46 00 00 48 8B F9 48 8B D3 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 8B "
        "D7 48 8B CB 48 89 07 E8 ?? ?? ?? ??",
        0x31787B0};
    inline const sig::Signature kAudioHolderVtableB{"ped_audio_holder_vtable_b", kAudioHolderCtorB,
        sig::Rel32{0x1F, 0x23}, "", 0x3839728};

    // The weather entity's initializer: lea rcx, [W] (the static object),
    // lea rax, [its vtable].
    inline const sig::Signature kWeatherEntityInit{"weather_entity_init",
        "48 83 EC 28 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? "
        "E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ??",
        0x112D8C};
    inline const sig::Signature kWeatherEntityVtable{"weather_entity_vtable", kWeatherEntityInit, sig::Rel32{0x13, 0x17},
        "", 0x383D3B0};

    // The sky update (its reads of the sky's and the timecycle's values).
    inline const sig::Signature kSkyUpdate{"sky_update",
        "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D A8 98 F8 FF FF", 0x5F8640};

    // The vehicle's break-off (its fragInst, the wheel break).
    inline const sig::Signature kVehicleBreakOff{"vehicle_break_off",
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 54 41 55 41 56 41 57 48 83 EC 50 0F 29 70 C8 45 33 ED 44 38 "
        "2D ?? ?? ?? ??",
        0x1579488};

    // The horse group audio caller: loads the group manager, calls the group
    // update, and clears the speech duck flags.
    inline const sig::Signature kSoundGroupCaller{"sound_group_caller",
        "40 53 48 83 EC 20 80 3D ?? ?? ?? ?? 00 8A D9 0F 84 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ??", 0x2EF1F50};

    // Sound-making functions: the collision code and, at +0x353, its load of
    // the static collision audio entity and its call of the persistent sound
    // creation (audio entity, descriptor, audSound** out, init params; a
    // twin differs only in its call targets, hence reached from here); the
    // melee impact entries; the footstep sounds and their helpers.
    inline const sig::Signature kAudioCollisionCode{"audio_collision_code",
        "48 8B C4 48 89 58 08 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 88 FE FF FF 48 81 EC 40 02 00 00 45 32 E4",
        0x2F09D0C};
    inline const sig::Signature kAudioCollisionSite{"audio_collision_site", kAudioCollisionCode, sig::Offset{0x353},
        "48 8D 0D ?? ?? ?? ?? 0F 29 45 A0 4C 8D 4D F0 4C 8B C6 E8 ?? ?? ?? ??", 0x2F0A05F};
    inline const sig::Signature kAudioPersistentCreate{"audio_persistent_create", kAudioCollisionSite,
        sig::Rel32{0x13, 0x17},
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 57 48 81 EC 60 01 00 00 48 8B E9 49 8B F9 48 8D 4C 24 20 49 8B F0 "
        "48 8B DA E8 ?? ?? ?? ??",
        0x2588FE8};
    inline const sig::Signature kAudioMeleeImpact{"audio_melee_impact",
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 8B D9 E8 ?? ?? ?? ?? 48 8B 4B 10 8B 57 48",
        0x318A28C};
    inline const sig::Signature kAudioMeleeBody{"audio_melee_body_impact",
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 55 41 56 41 57 48 8D 68 B9 48 81 EC C8 00 00 00 0F 29 70 A8",
        0x318EC48};
    inline const sig::Signature kAudioStepLayers{"audio_step_layers",
        "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 56 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 0F 29 70 D8 49 "
        "8B F0",
        0x318CEB4};
    inline const sig::Signature kAudioStepSound{"audio_step_sound",
        "48 8B C4 48 89 58 18 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 48 FD FF FF", 0x318D52C};
    inline const sig::Signature kAudioStepCreate{"audio_step_create",
        "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8D 6C 24 90", 0x318E6E8};
    inline const sig::Signature kAudioFootHelperA{"audio_foot_helper_a",
        "48 89 5C 24 18 48 89 74 24 20 55 57 41 56 48 8D AC 24 70 FF FF FF 48 81 EC 90 01 00 00 48 8B D9", 0x318E9C4};
    inline const sig::Signature kAudioStepOther{"audio_step_other", "48 89 5C 24 08 57 48 83 EC 30 49 83 38 00",
        0x318FC04};
    inline const sig::Signature kAudioFootHelperB{"audio_foot_helper_b", "48 8B C4 89 50 10 55 53 56 57 41 54",
        0x318C150};

    // The movement volume of a ped's audio (the mount audio calls it too).
    inline const sig::Signature kMovementVolume{"audio_movement_volume",
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? EC 50 83 60 18 00 48 8B D9 0F 29 70 E8 0F 28 F3 0F 29 78 D8 0F 57 FF",
        0x31815C4};

    // "MixerStartScene" (mixer, &hash, ...) and "MixerStopScene" (scene, byte).
    inline const sig::Signature kMixerStartScene{"mixer_start_scene",
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 41 56 48 83 EC 40 49 8B F0 4C 8B F2 8B 12 48 8B F9 41 B0 03 48 8D "
        "0D ?? ?? ?? ?? 49 8B D9 E8 ?? ?? ?? ?? 4C 8B 94 24 80 00 00 00 4C 8B C6 44 8B 4C 24 70 48 8B D0 4C 89 54 24 30 "
        "48 8B CF 4C 8B 54 24 78",
        0x2F19018};
    inline const sig::Signature kMixerStopScene{"mixer_stop_scene",
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 48 89 78 20 41 56 48 83 EC 70 0F 29 70 E8 48 8D 79 70 40 8A EA",
        0x327A07C};

    // The conversation duck's start: its call of the mixer start at +0x130
    // passes the conversation's +0xE8 as the scene's out pointer.
    inline const sig::Signature kConversationDuckStart{"conversation_duck_start",
        "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC C0 00 00 00 48 8D B1 E8 00 00 00",
        0x2FC2200};
    inline const sig::Signature kConversationSceneCall{"conversation_scene_call", kConversationDuckStart,
        sig::Offset{0x130}, "E8 ?? ?? ?? ??", 0x2FC2330};
    inline const sig::Signature kConversationSceneStart{"conversation_scene_start", kConversationSceneCall,
        sig::Rel32{0x01, 0x05},
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 41 56 48 83 EC 40 49 8B F0 4C 8B F2 8B 12 48 8B F9 41 B0 03 48 8D "
        "0D ?? ?? ?? ?? 49 8B D9 E8 ?? ?? ?? ?? 4C 8B 94 24 80 00 00 00 4C 8B C6 44 8B 4C 24 70 48 8B D0 4C 89 54 24 30 "
        "48 8B CF 4C 8B 54 24 78",
        0x2F19018};

    // The speech duck driver, its prologue up to its first branch: mov rax,rsp;
    // two saves; push rbp; lea rbp,[rax-5Fh]; sub rsp,0D0h; movaps; xor
    // esi,esi; cmp [skip gate],sil; five movaps; jne.
    inline const sig::Signature kDuckDriver{"duck_driver",
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 68 A1 48 81 EC D0 00 00 00 0F 29 78 E8 33 F6 40 38 35 ?? ?? ?? ?? 44 "
        "0F 29 40 D8 44 0F 29 48 C8 44 0F 29 50 B8 44 0F 29 58 A8 44 0F 29 60 98 44 0F 29 68 88 0F 85 ?? ?? ?? ??",
        0x2F2D1EC};
}
