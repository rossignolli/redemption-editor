#pragma once

// The game addresses a clip names by signature (game_sig_site.h): the places
// the playback compares a recorded address with. A clip writes these fields
// as site ids and reads them back as RVAs of the running game, so a clip
// keeps its fx sites, callers and audio objects across game builds
// (docs/signatures.md).
//
// The ranges: the fx call sites (fx_observer.h RecordedSites, the fire and
// weapon sites below), the decal and blood callers, the speech line's sound
// calls, the sound owner's functions (sound_owner_table.h kCallerFunctions),
// the audio entity's wrappers (sound_generic_core.h WrapperCode), the static
// audio objects and their vtables (sound_check_core.h) and the audio entity
// vtables the recorder sees on peds and vehicles. The signatures some modules
// declared in their .cpp are here so the clip tools have them too. Pure;
// unit tested in tests/game_sig_site_tests.cpp.

#include "fx_motion_rule.h"
#include "fx_observer.h"
#include "game_sig_core.h"
#include "game_sig_site.h"
#include "script_fx_loop.h"
#include "shot_fx_audit_core.h"
#include "sound_check_core.h"
#include "sound_generic_core.h"
#include "sound_owner_table.h"
#include "speech_conversation.h"
#include "weapon_fx_intent_core.h"

#include <cstddef>
#include <cstdint>

namespace replay::events
{
    // Registered fx the fire system emits: real fires bring them back, so
    // their recorded copies do not play. Every registered site reached from
    // the fire code, not only the area flames and smoulder: the entity fire
    // flames and smoke of the area update and the ped limb flames of
    // CFire::Update. Played as copies on top of the real fire, a burnt
    // wagon's smoke track would be drawn on the coach horse after the fire
    // went out. A site is the return address of the fx start call (call +
    // 5) in its function.
    inline const sig::Signature kFireAreaFxHost{"events_fire_area_fx_host",
        "48 89 5C 24 08 55 56 57 41 56 41 57 48 8D 6C 24 D1 48 81 EC 90 00 00 00 41 8B 01 48 8B F2", 0x1466A50};
    inline const sig::Signature kFireAreaFxCall{"events_fire_area_fx_call", kFireAreaFxHost, sig::Offset{0x46},
        "E8 ?? ?? ?? ??", 0x1466A96};
    inline const sig::Signature kFireSmoulderFxHost{"events_fire_smoulder_fx_host",
        "48 89 5C 24 08 55 56 57 41 56 41 57 48 8D 6C 24 D1 48 81 EC 90 00 00 00 41 8B 01 4C 8B F2", 0x1466C98};
    inline const sig::Signature kFireSmoulderFxCall{"events_fire_smoulder_fx_call", kFireSmoulderFxHost, sig::Offset{0x46},
        "E8 ?? ?? ?? ??", 0x1466CDE};
    inline const sig::Signature kFireEntityFlamesFxHost{"events_fire_entity_flames_fx_host",
        "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 F1 48 81 EC A0 00 00 00 4C 8B 15 ?? ?? ?? ??",
        0x1463A48};
    inline const sig::Signature kFireEntityFlamesFxCall{"events_fire_entity_flames_fx_call", kFireEntityFlamesFxHost,
        sig::Offset{0xC9}, "E8 ?? ?? ?? ??", 0x1463B11};
    inline const sig::Signature kFireEntitySmokeFxHost{"events_fire_entity_smoke_fx_host",
        "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D 68 A9 48 81 EC B0 00 00 00 48 "
        "8B BA 10 01 00 00", 0x1467D9C};
    inline const sig::Signature kFireEntitySmokeFxCall{"events_fire_entity_smoke_fx_call", kFireEntitySmokeFxHost,
        sig::Offset{0xAA}, "E8 ?? ?? ?? ??", 0x1467E46};
    inline const sig::Signature kFireLimbFlamesFxHost{"events_fire_limb_flames_fx_host",
        "48 8B C4 48 89 58 08 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 48 FD FF FF", 0x14640A8};
    inline const sig::Signature kFireLimbFlamesFxCall{"events_fire_limb_flames_fx_call", kFireLimbFlamesFxHost,
        sig::Offset{0x3C4}, "E8 ?? ?? ?? ??", 0x146446C};
    // Two weapon fx sites besides the fx observer's muzzle, impact and body.
    inline const sig::Signature kWeaponFxAHost{"events_weapon_fx_a_host",
        "48 8B C4 48 89 58 08 4C 89 40 18 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 18 FF FF FF 48 81 EC B0 01 00 00 "
        "0F 29 70 B8 49 8B F9", 0x14606E0};
    inline const sig::Signature kWeaponFxACall{"events_weapon_fx_a_call", kWeaponFxAHost, sig::Offset{0x2A8},
        "E8 ?? ?? ?? ??", 0x1460988};
    inline const sig::Signature kWeaponFxBHost{"events_weapon_fx_b_host",
        "4D 85 C9 0F 84 ?? ?? ?? ?? 48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 57", 0x146798C};
    inline const sig::Signature kWeaponFxBCall{"events_weapon_fx_b_call", kWeaponFxBHost, sig::Offset{0x15B},
        "E8 ?? ?? ?? ??", 0x1467AE7};
}

namespace replay::decal
{
    // The generic wrapper (manager, entity, settings, flag): allocates the id,
    // then calls the core (the return recorded as the generic path).
    inline const sig::Signature kDecalWrapper{"decal_wrapper",
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 30 41 8A F1 49 8B E8 4C 8B F2 48 8B "
        "D9",
        0x2C2F054};
    inline const sig::Signature kDecalGenericCall{"decal_generic_call", kDecalWrapper, sig::Offset{0x41},
        "E8 ?? ?? ?? ?? 85 C0 75 0C", 0x2C2F095};
}

namespace replay::blood
{
    // The record merge the add path calls (set, ..., record copy, ...).
    inline const sig::Signature kBloodMerge{"blood_merge",
        "48 8B C4 48 89 58 10 48 89 68 18 48 89 70 20 57 41 54 41 55 41 56 41 57 48 83 EC 70 0F 29 70 C8 48 8B F9",
        0x125D86C};
    // Its evolve's call of the finalize (blood_evolve_write in ped_blood.cpp).
    inline const sig::Signature kBloodEvolveCall{"blood_evolve_call", kBloodMerge, sig::Offset{0x552},
        "E8 ?? ?? ?? ?? 83 A3 10 01 00 00 00", 0x125DDBE};
    // The bullet blood, a caller of the finalize.
    inline const sig::Signature kBloodBullet{"blood_bullet",
        "48 8B C4 4C 89 48 20 4C 89 40 18 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 48 F6 FF FF",
        0x125DE00};
    inline const sig::Signature kBloodBulletCall{"blood_bullet_call", kBloodBullet, sig::Offset{0xCFA},
        "E8 ?? ?? ?? ?? 48 8B C6 EB 02", 0x125EAFA};
    // The add path (manager, ped set slot, record copy, refuse) -> record.
    inline const sig::Signature kBloodAdd{"blood_add",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 45 8A D1 4D "
        "8B E8 44 8A F2 80 FA FF 0F 84 ?? ?? ?? ?? 44 0F B6 E2 33 F6 4E 8B BC E1 D8 00 00 00",
        0x125D470};
    // Its call of the finalize (the damage packs' records).
    inline const sig::Signature kBloodPackCall{"blood_pack_call", kBloodAdd, sig::Offset{0x18F},
        "E8 ?? ?? ?? ?? 48 8B C5 EB 02", 0x125D5FF};
}

namespace replay::clipsites
{
    // The audio entity vtables a clip's generic and general sounds carry
    // (SGEN, GSND vtable), besides the static objects' own: the store of the
    // vtable in each class's constructor (sigtool xref of the vtables seen in
    // the clips 1283 and 1285). The two ped holder classes are twins, byte
    // for byte, constructors included: each is reached from its one caller.
    inline const sig::Signature kPedAudioACaller{"clip_ped_audio_a_caller",
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8A 05 ?? ?? ?? ?? 48 8B F1 84 C0 75 04 33 D2 EB 36 48 8B 0D ?? ?? "
        "?? ?? 48 8B 15 ?? ?? ?? ?? 48 C1 C1 20",
        0x89E4F8};
    inline const sig::Signature kPedAudioACall{"clip_ped_audio_a_ctor_call", kPedAudioACaller, sig::Offset{0xE3},
        "E8 ?? ?? ?? ?? EB 02 33 FF 48 89 3E", 0x89E5DB};
    inline const sig::Signature kPedAudioACtor{"clip_ped_audio_a_ctor", kPedAudioACall, sig::Rel32{0x01, 0x05}, "",
        0x31787B0};
    inline const sig::Signature kPedAudioARef{"clip_ped_audio_a_vtable_ref", kPedAudioACtor, sig::Offset{0x1C},
        "48 8D 05 ?? ?? ?? ?? 48 8B D7 48 8B CB 48 89 07 E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B C7 48 83 C4 20 5F C3",
        0x31787CC};
    inline const sig::Signature kPedAudioA{"clip_ped_audio_a_vtable", kPedAudioARef, sig::Rel32{0x03, 0x07}, "",
        0x3839728};
    inline const sig::Signature kPedAudioBCaller{"clip_ped_audio_b_caller",
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8A 05 ?? ?? ?? ?? 48 8B F1 84 C0 75 04 33 D2 EB 37",
        0x89DAC8};
    inline const sig::Signature kPedAudioBCall{"clip_ped_audio_b_ctor_call", kPedAudioBCaller, sig::Offset{0xE5},
        "E8 ?? ?? ?? ?? EB 02 33 FF 48 89 3E", 0x89DBAD};
    inline const sig::Signature kPedAudioBCtor{"clip_ped_audio_b_ctor", kPedAudioBCall, sig::Rel32{0x01, 0x05}, "",
        0x31786FC};
    inline const sig::Signature kPedAudioBRef{"clip_ped_audio_b_vtable_ref", kPedAudioBCtor, sig::Offset{0x1C},
        "48 8D 05 ?? ?? ?? ?? 48 8B D7 48 8B CB 48 89 07 E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B C7 48 83 C4 20 5F C3",
        0x3178718};
    inline const sig::Signature kPedAudioB{"clip_ped_audio_b_vtable", kPedAudioBRef, sig::Rel32{0x03, 0x07}, "",
        0x3839630};
    inline const sig::Signature kPedAudioCRef{"clip_ped_audio_c_vtable_ref",
        "48 8D 05 ?? ?? ?? ?? 48 8D 4F 08 48 89 07 E8 ?? ?? ?? ?? 48 89 77 18", 0x2320D27};
    inline const sig::Signature kPedAudioC{"clip_ped_audio_c_vtable", kPedAudioCRef, sig::Rel32{0x03, 0x07}, "",
        0x330D5E0};
    inline const sig::Signature kVehicleAudioARef{"clip_vehicle_audio_a_vtable_ref",
        "48 8D 05 ?? ?? ?? ?? B9 08 00 00 00 48 89 03 0F 57 DB", 0x31004AC};
    inline const sig::Signature kVehicleAudioA{"clip_vehicle_audio_a_vtable", kVehicleAudioARef, sig::Rel32{0x03, 0x07},
        "", 0x3835770};
    inline const sig::Signature kVehicleAudioBRef{"clip_vehicle_audio_b_vtable_ref",
        "48 8D 05 ?? ?? ?? ?? 48 8D 8B 30 0C 00 00 48 89 03 E8 ?? ?? ?? ?? 48 8D 8B 44 0C 00 00", 0x30FF956};
    inline const sig::Signature kVehicleAudioB{"clip_vehicle_audio_b_vtable", kVehicleAudioBRef, sig::Rel32{0x03, 0x07},
        "", 0x38343E0};
    inline const sig::Signature kEntityAudioARef{"clip_entity_audio_a_vtable_ref",
        "48 8D 05 ?? ?? ?? ?? 48 89 03 48 8D 0D ?? ?? ?? ?? 48 89 93 C8 01 00 00", 0x3100394};
    inline const sig::Signature kEntityAudioA{"clip_entity_audio_a_vtable", kEntityAudioARef, sig::Rel32{0x03, 0x07}, "",
        0x3833068};

    // A range: a signature's own bytes, or with a tail its whole function
    // (to the end of the tail's bytes).
    struct Span
    {
        const sig::Signature* sig;
        const sig::Signature* tail = nullptr;
    };

    inline const Span kSpans[] = {
        // fx call sites
        {&weaponfx::kWeaponMuzzleCall}, {&weaponfx::kWeaponShellCall}, {&shotfx::kShotImpactCall},
        {&shotfx::kShotBodyCall}, {&fx::kFxFootVfxCall}, {&fx::kFxCameraAmbientCall}, {&scriptfx::kScriptFxStartCall},
        {&scriptfx::kScriptFxKeepAliveCall}, {&fxmotion::kFxHoofCall}, {&fxmotion::kFxRegistryRestartCall},
        {&events::kFireAreaFxCall}, {&events::kFireSmoulderFxCall}, {&events::kFireEntityFlamesFxCall},
        {&events::kFireEntitySmokeFxCall}, {&events::kFireLimbFlamesFxCall}, {&events::kWeaponFxACall},
        {&events::kWeaponFxBCall},
        // decal and blood callers
        {&decal::kDecalGenericCall}, {&blood::kBloodPackCall}, {&blood::kBloodBulletCall}, {&blood::kBloodEvolveCall},
        // the speech line's sound calls (entity sound callers)
        {&speech::conversation::kWaveSoundCall}, {&speech::conversation::kLayerSoundCall},
        // the audio entity's wrappers (soundgeneric::KnownWrapperCode)
        {&soundgeneric::kCreatorEntries, &soundgeneric::kCreatorATail},
        {&soundgeneric::kWrappers, &soundgeneric::kQueueDrainTail},
        {&soundgeneric::kOtherDrain, &soundgeneric::kOtherDrainTail},
        {&soundgeneric::kQueueDrain, &soundgeneric::kQueueDrainTail},
        // audio entity vtables
        {&kPedAudioA}, {&kPedAudioB}, {&kPedAudioC}, {&kVehicleAudioA}, {&kVehicleAudioB}, {&kEntityAudioA},
    };

    // One build's map. `rvaOf(signature)`: its RVA on that build, 0 when not
    // found. Besides kSpans: the sound owner's functions and the static audio
    // objects with their vtables.
    template <typename RvaOf>
    sig::SiteMap BuildSiteMap(RvaOf&& rvaOf)
    {
        sig::SiteMap map;
        const auto add = [&](const sig::Signature& s, const sig::Signature* tail) {
            const std::uint32_t begin = rvaOf(s);
            const sig::Signature& last = tail ? *tail : s;
            const std::uint32_t lastAt = tail ? rvaOf(*tail) : begin;
            sig::Pattern pattern;
            const std::uint32_t size = sig::Parse(last.pattern, pattern) ? static_cast<std::uint32_t>(pattern.size) : 0u;
            if (begin && lastAt)
                map.Add(sig::NameHash(s.name), begin, lastAt + size);
        };
        for (const Span& span : kSpans)
            add(*span.sig, span.tail);
        for (const soundowner::CallerFunction& f : soundowner::kCallerFunctions)
            add(*f.start, f.tail);
        for (const census::ImageAudioSignatures& s : census::kImageAudioSignatures)
        {
            add(*s.object, nullptr);
            if (s.vtable)
                add(*s.vtable, nullptr);
        }
        return map;
    }

    // The 1491.50 exe's map: the signatures' declared RVAs. What a clip
    // written before the site ids holds, and what the offline tools read.
    inline const sig::SiteMap& KnownSiteMap()
    {
        static const sig::SiteMap map = BuildSiteMap([](const sig::Signature& s) { return s.rva; });
        return map;
    }
}
