// audio_observer_sounds.cpp: sounds played: fires, sound requests, direct and persistent sounds, sound variables, melee and explosion audio, the puppet silencing, anim events, the census call sites.
// Split from audio_observer.cpp (moved code); shared internals in audio_observer_internal.h.

#include "audio_observer_internal.h"

namespace replay::audio
{
    namespace
    {
        // The melee "definition" is the engine's action result (the melee move
        // from the action tables; +0x48 is its name hash, e.g.
        // AR_GRAPPLE_ATTACK 0x0C500859; +0x30 is the material set the impact
        // audio picks from). The engine finds one by that hash with the action
        // result lookup (manager, hash): a binary search of the sorted (hash,
        // result) map at manager +0x30, no side effects. TASK_MELEE's builder
        // does exactly that at the site below (mov rcx,[rip+manager]; mov
        // edx,r15d; call), and so do the melee clone tasks; a melee event
        // keeps only the name hash.
        const sig::Signature kMeleeBuilder{"audio_melee_task_builder",
            "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 56 41 57 48 8D A8 08 FF FF FF 48 81 EC E0 01 00 00 33 "
            "FF", 0x1096C7C};
        const sig::Signature kActionResultSite{"audio_action_result_site", kMeleeBuilder, sig::Offset{0xC4},
            "48 8B 0D ?? ?? ?? ?? 41 8B D7 E8 ?? ?? ?? ??", 0x1096D40};
        const sig::Signature kActionResultManager{"audio_action_result_manager", kActionResultSite,
            sig::Rel32{0x3, 0x7}, "", 0x3EBFEC8};
        const sig::Signature kActionResultLookup{"audio_action_result_lookup", kActionResultSite, sig::Rel32{0xB, 0xF},
            "48 83 EC 28 89 54 24 38 48 83 C1 30 48 8D 54 24 38 E8 ?? ?? ?? ??", 0xCE0724};
        // The call of the by-ref core inside the collision entry: its return
        // address is the collision track's caller.
        const sig::Signature kCollisionCreateCall{"audio_collision_create_call", kAudioPersistentCreate,
            sig::Offset{0x43},
            "E8 ?? ?? ?? ??", 0x258902B};
        const sig::Signature kRequestedSettingsStride{"audio_requested_settings_stride", kStopAndForget,
            sig::Rel32{0x20, 0x24}, "", 0x53588B4};
        const sig::Signature kRequestedSettingsTable{"audio_requested_settings_table", kStopAndForget,
            sig::Rel32{0x2E, 0x32}, "", 0x53588D8};
        const sig::Signature kGetVariable{"audio_get_variable",
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 49 8B F8 8B F2 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0", 0x2573738};
        using SetVariableFn = bool(__fastcall*)(std::uint64_t, const std::uint32_t*, float);
        // S38 (the variable setter, slot 1 of the sound vtables) refuses a
        // value that is not finite and a hash its lookup (the variable find)
        // does not find: the top sound (the root sound climbs +0xE2, the
        // parent's slot) holds the block at +0x9C (0xFFFF: none), 16 (hash,
        // value) pairs and the count at +0x80, in the sound pool (the table,
        // bucket +0xA0 times 0x2A860, +0x2A848; the slot stride). The lookup
        // never adds: only a variable block sound's Init registers the
        // variables it exports. The reason of a refusal is read with these.
        const sig::Signature kVariableFind{"audio_variable_find",
            "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 74 57 44 0F B7 80 9C 00 00 00 B8 FF "
            "FF 00 00 66 44 3B C0 74 44 0F B6 83 A0 00 00 00 48 69 C8 60 A8 02 00 48 8B 05 ?? ?? ?? ?? 41 0F B7 D0 0F "
            "AF 15 ?? ?? ?? ?? 48 03 94 01 48 A8 02 00", 0x25736B4};
        const sig::Signature kSoundPoolTable{"audio_sound_pool_table", kVariableFind, sig::Rel32{0x3D, 0x41},
            "", 0x53588D8};
        const sig::Signature kSoundSlotStride{"audio_sound_slot_stride", kVariableFind, sig::Rel32{0x48, 0x4C},
            "", 0x53588B0};
        const sig::Signature kRootSound{"audio_root_sound",
            "0F B7 91 E2 00 00 00 4C 8B C1 4C 8B 15 ?? ?? ?? ?? 44 8B 1D ?? ?? ?? ??", 0x25745A8};
        const sig::Signature kSoundVariableSet{"audio_sound_variable_set",
            "48 83 EC 38 41 B8 00 00 80 7F 0F 29 74 24 20", 0x25735EC};
        // Requested-settings position setter (RS, Vec4*): mov eax,[rip+X]
        // then the fixed body; the displacement is masked.
        const sig::Signature kSetPosition{"audio_set_position",
            "8B 05 ?? ?? ?? ?? 0F 10 02 48 FF C0 48 03 C0 0F 11 04 C1", 0x2536E48};
        const sig::Signature kExplosionTagRef{"audio_explosion_tag_ref",
            "40 53 48 83 EC 20 48 8B D9 83 FA 24", 0x2EF10D8};
        const sig::Signature kExplosionPlay{"audio_explosion_play",
            "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8B EC 48 83 EC 60 83 B9 00 04 00 00 10 41 "
            "8A F1 4D 8B F0 48 8B F9 74 7C 41 B0 65 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 65 8B 50 "
            "30 48 8D 0D ?? ?? ?? ?? 41 B0 B1 E8 ?? ?? ?? ?? 48 85 C0 74 4E 41 0F 10 06 48 8D 55 C0 48 83 65 D8 00 48 "
            "8B CF 48 83 65 F8 00 48 89 45 C8 8A 45 30 88 45 F1 8B 45 38 89 45 F4 8A 45 40",
            0x2EF01EC};
        // The manager the explosion native hands the explosion play (its call:
        // engine_calls.h kExplosionPlay).
        const sig::Signature kExplosionNative{"audio_explosion_native",
            "40 53 48 83 EC 50 48 8B DA 33 D2 E8 ?? ?? ?? ??", 0xF66964};
        const sig::Signature kExplosionManagerLea{"audio_explosion_manager_lea", kExplosionNative, sig::Offset{0x76},
            "48 8D 0D ?? ?? ?? ??", 0xF669DA};
        const sig::Signature kExplosionManager{"audio_explosion_manager", kExplosionManagerLea, sig::Rel32{0x3, 0x7},
            "", 0x5B9FC30};
        // AddAnimEvent (audio, event*): slot +0xB8 of the three ped audio
        // vtables (entity_query.h kHolderVtables); returns when [+0x14] == 0 or
        // the count [+0xD8] is 4 or its search finds the same {extra, hash};
        // registers the entity when [+0x10] == 0xFFFF; copies the 0x18 bytes to
        // +0x78 + count * 0x18. The prologue plus the first compare (34 bytes)
        // is unique in .text.
        const sig::Signature kAddAnimEvent{"audio_add_anim_event",
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 83 79 14 00 48 8B F2 48 8B D9 74 53 83 B9 D8 00 00 00 04",
            0x2C2D934};
        // vehicle_sound_rule.h: the puppet vehicles' and train cars' audio
        // entities ([vehicle + 0x900], span 0x1800 as the census's
        // kVehicleAudioSpan) with their clip entities, the switch (ini
        // SoundGenericVehicleBlock) and the counts. Written by the script
        // thread (the clip entity before the entity, as the holders'), read
        // by the census creators on any thread.
        constexpr std::size_t kMaxSilencedVehicles = 128;
        constexpr std::uint64_t kSilencedVehicleSpan = 0x1800;
        std::atomic<std::uint64_t> g_silencedVehicleAudio[kMaxSilencedVehicles] = {};
        std::atomic<std::uint32_t> g_silencedVehicleOwner[kMaxSilencedVehicles] = {};
        std::atomic<std::uint32_t> g_silencedVehicleCount{0};
        std::atomic<bool> g_vehicleOwnBlock{true};
        std::atomic<std::uint64_t> g_vehicleOwnRefused{0};
        std::atomic<std::uint64_t> g_vehicleOwnPassed{0};

        // The listed vehicle audio entity `audio` lies in, with its clip entity.
        bool SilencedVehicleAudioOwner(std::uint64_t audio, std::uint32_t& owner)
        {
            const std::uint32_t count = g_silencedVehicleCount.load(std::memory_order_acquire);
            if (!count || !audio)
                return false;
            const std::uint32_t used = count < kMaxSilencedVehicles ? count : static_cast<std::uint32_t>(kMaxSilencedVehicles);
            for (std::uint32_t n = 0; n < used; ++n)
            {
                const std::uint64_t entity = g_silencedVehicleAudio[n].load(std::memory_order_relaxed);
                if (!entity || audio < entity || audio - entity >= kSilencedVehicleSpan)
                    continue;
                owner = g_silencedVehicleOwner[n].load(std::memory_order_relaxed);
                return true;
            }
            return false;
        }
    }

    void CollectFires(std::uint64_t fromUs, std::uint64_t toUs, std::vector<FireCall>& out)
    {
        Collect(g_fires, kFireRing, g_nextFire, fromUs, toUs, out);
    }
    namespace
    {

        bool GuardedEnqueue(std::uintptr_t fn, const std::uint8_t* info, std::uint32_t& code)
        {
            return engine::Call(engine::kFireEnqueue, fn, info) || engine::Failed(code);
        }
    }

    bool EnqueueFire(const std::uint8_t info[kFireInfoBytes], std::string& error)
    {
        // Through the trampoline when the observer hooked it, so the replayed
        // event is not recorded again.
        std::uintptr_t fn = reinterpret_cast<std::uintptr_t>(g_originalFire.load(std::memory_order_acquire));
        if (!fn)
        {
            if (!g_fireDirect)
            {
                g_fireDirect = sig::Find(kFireEnqueue);
                if (!g_fireDirect)
                {
                    error = "fire_enqueue_signature_mismatch";
                    return false;
                }
            }
            fn = g_fireDirect;
        }
        alignas(16) std::uint8_t copy[kFireInfoBytes];
        std::memcpy(copy, info, kFireInfoBytes);
        std::uint32_t code = 0;
        if (!GuardedEnqueue(fn, copy, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        return true;
    }
    namespace
    {

    // The player's weapon settings for a puppet's weapon (see the header).
    // The engine's own set-up is not usable on the puppet: its owner lookup
    // (+0x1C8 -> +0xF0 -> vfunc +0x58) does not give the puppet ped. So the
    // same lookup it ends in is called here with the player variant. Settings
    // by weapon hash and variant (type 0x37 is the weapon audio item, 0x15 the
    // settings, both through the metadata reference lookup):
    //   hash-1 > -3 -> 0; item = find(hash, 0x37); variant ? item field
    //   `variant` : [item+4]; settings = find(that, 0x15)
    // The settings getter returns the cached settings at +0x1F8 (the field
    // written, kWeaponSettingsCached) or asks the lookup with the item hash
    // +0x1E0 and variant 0.
        const sig::Signature kWeaponSettingsLookup{"audio_weapon_settings_lookup",
            "48 89 5C 24 10 57 48 83 EC 20 8D 41 FF 8B FA 83 F8 FD 77 50 8B D1 41 B0 37 48 8D 0D ?? ?? ?? ?? E8 ?? ?? "
            "?? ?? 48 8B D8 48 85 C0 74 37 85 FF 74 1F 44 8B C7 48 8D 4C 24 30 48 8B D0 E8 ?? ?? ?? ?? 8B 10 44 8D 42 "
            "01 41 F7 C0 FE FF FF FF 75 03 8B 53 04 41 B0 15 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? EB 02 33 C0 48 8B 5C "
            "24 38 48 83 C4 20 5F C3", 0x2EF1D04};
        const sig::Signature kWeaponSettingsGetter{"audio_weapon_settings_getter",
            "40 53 48 83 EC 20 8B 05 ?? ?? ?? ?? 48 8B D9 C1 E8 04", 0x311971C};
        const sig::Signature kWeaponSettingsCached{"audio_weapon_settings_cached", kWeaponSettingsGetter,
            sig::Offset{0x3B}, "48 8B 83 F8 01 00 00", 0x3119757};
        constexpr std::uint32_t kPlayerWeaponSettingsVariant = 0x6F0783F5; // the settings variant's
        constexpr std::size_t kWeaponAudioOffset = 0x100;     // CWeapon -> its weapon audio
        constexpr std::size_t kWeaponAudioOwner = 0x1C8;      // set up: the weapon the audio belongs to
        constexpr std::size_t kWeaponAudioHash = 0x1E0;       // weapon audio item hash
        constexpr std::size_t kWeaponAudioSettings = 0x1F8;   // cached settings (kWeaponSettingsCached)
        std::uintptr_t g_weaponSettingsLookup = 0;

        bool GuardedSettingsLookup(std::uintptr_t fn, std::uint32_t hash, std::uint32_t variant, std::uint64_t& out,
            std::uint32_t& code)
        {
            return engine::Call(engine::kWeaponSettingsLookup, fn, out, hash, variant) || engine::Failed(code);
        }

        bool GuardedWriteU64(std::uint64_t address, std::uint64_t value)
        {
            __try
            {
                *reinterpret_cast<volatile std::uint64_t*>(address) = value;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool RefreshWeaponAudioSettings(std::uint64_t cweapon, std::uint64_t& before, std::uint64_t& after,
        std::string& error)
    {
        before = after = 0;
        if (!cweapon)
        {
            error = "no_weapon";
            return false;
        }
        if (!g_weaponSettingsLookup)
        {
            // No write without the engine's own read of the field written.
            const std::uintptr_t lookup = sig::Find(kWeaponSettingsCached) ? sig::Find(kWeaponSettingsLookup) : 0;
            if (!lookup)
            {
                error = "weapon_settings_lookup_signature_mismatch";
                return false;
            }
            g_weaponSettingsLookup = lookup;
        }
        const std::uint64_t audio = cweapon + kWeaponAudioOffset;
        std::uint64_t owner = 0;
        std::uint32_t hash = 0;
        if (!ReadU64(audio + kWeaponAudioOwner, owner) || !owner || !ReadU64(audio + kWeaponAudioSettings, before) ||
            !GuardedRead(&hash, reinterpret_cast<const void*>(audio + kWeaponAudioHash), 4) || !hash)
        {
            error = "weapon_audio_not_set_up";
            return false;
        }
        std::uint64_t npc = 0;
        std::uint64_t player = 0;
        std::uint32_t code = 0;
        if (!GuardedSettingsLookup(g_weaponSettingsLookup, hash, 0, npc, code) ||
            !GuardedSettingsLookup(g_weaponSettingsLookup, hash, kPlayerWeaponSettingsVariant, player, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        after = before;
        // Only the NPC settings are replaced (a weapon set up any other way,
        // e.g. by the camera branch of the settings getter, is left alone).
        if (!player || before != npc)
        {
            error = !player ? "no_player_settings" : "not_npc_settings";
            return false;
        }
        if (player != before && !GuardedWriteU64(audio + kWeaponAudioSettings, player))
        {
            error = "write_failed";
            return false;
        }
        ReadU64(audio + kWeaponAudioSettings, after);
        error = "none";
        return true;
    }

    std::uint64_t PedAudio(void* pedEntity)
    {
        std::uint8_t* audio = nullptr;
        if (!pedEntity || !GuardedPedAudio(pedEntity, audio))
            return 0;
        return reinterpret_cast<std::uint64_t>(audio);
    }

    void CollectFiresSince(std::uint64_t& cursor, std::vector<FireCall>& out)
    {
        CollectSince(g_fires, kFireRing, g_nextFire, cursor, out);
    }

    std::uint64_t TotalFires()
    {
        return g_nextFire.load(std::memory_order_relaxed);
    }

    void CollectSpeechCoreSince(std::uint64_t& cursor, std::vector<SpeechCoreCall>& out)
    {
        CollectSince(g_core, kCoreRing, g_nextCore, cursor, out);
    }

    void CollectImpacts(std::uint64_t fromUs, std::uint64_t toUs, std::vector<ImpactCall>& out)
    {
        Collect(g_impacts, kImpactRing, g_nextImpact, fromUs, toUs, out);
    }

    void CollectSoundRequestsSince(std::uint64_t& cursor, std::vector<SoundRequest>& out)
    {
        CollectSince(g_soundRequests, kSoundRequestRing, g_nextSoundRequest, cursor, out);
    }

        // Recorded init params over constructed ones: the value fields only.
        // +0xC8 tracker, +0xD0 wave slot, +0xD8 category, +0xE0 environment
        // group (ref-counted or engine-owned) and the u16 handle at +0x12C
        // stay as the constructor left them. The tail [0x12E, 0x140) holds
        // value fields with non-zero defaults (its ctor: +0x130 dword -1,
        // +0x134 word -1, +0x136 dword 0xFF, +0x13A byte 0x20, +0x13B low bits
        // 0). Older clips wrote it as zeros, which are not what the game
        // played (the ids became 0 and the flags byte lost 0x20): an all-zero
        // tail is taken as not recorded and the constructor's values are kept.
        void CopyRecordedSoundParams(std::uint8_t* params, const std::uint8_t* recorded)
        {
            std::memcpy(params, recorded, 0xC8);
            std::memcpy(params + 0xE8, recorded + 0xE8, 0x12C - 0xE8);
            // The recorded category when this process knows it.
            std::uint64_t category = 0;
            std::memcpy(&category, recorded + kSoundCategoryOffset, sizeof(category));
            if (KnownSoundCategory(category))
                std::memcpy(params + kSoundCategoryOffset, &category, sizeof(category));
            bool tailRecorded = false;
            for (std::size_t offset = 0x12E; offset < kSoundParamsBytes && !tailRecorded; ++offset)
                tailRecorded = recorded[offset] != 0;
            if (tailRecorded)
                std::memcpy(params + 0x12E, recorded + 0x12E, kSoundParamsBytes - 0x12E);
        }
    namespace
    {

        bool GuardedSoundRequest(std::uintptr_t play, std::uintptr_t construct, std::uintptr_t release,
            std::uint64_t audio, std::uint64_t soundRef, std::uint64_t owner, const std::uint8_t* recorded,
            const std::uint64_t flags[4], bool& queued, std::uint32_t& code)
        {
            __try
            {
                alignas(16) std::uint8_t params[kSoundParamsBytes + 0x40] = {};
                std::uint64_t paramsArg = 0;
                if (recorded)
                {
                    void* constructed = nullptr;
                    if (!engine::Call(engine::kSoundParamsCtor, construct, constructed, params))
                        return engine::Failed(code);
                    CopyRecordedSoundParams(params, recorded);
                    paramsArg = reinterpret_cast<std::uint64_t>(params);
                }
                if (!engine::Call(engine::kSoundRequest, play, queued, audio, soundRef, owner, paramsArg, flags[0], flags[1],
                        flags[2], flags[3]))
                    return engine::Failed(code);
                if (recorded && !engine::Call(engine::kSoundParamsRelease, release, params + 0x12C))
                    return engine::Failed(code);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool PlaySoundRequest(void* pedEntity, std::uint64_t audioOffset, std::uint64_t soundRef,
        const std::uint8_t* params, const std::uint64_t flags[4], std::string& error)
    {
        // The constructor is observed: its prologue is patched, so the
        // detour's trampoline (the original code) is used when installed.
        static std::uintptr_t release = 0;
        std::uintptr_t construct = reinterpret_cast<std::uintptr_t>(g_originalCtor.load(std::memory_order_acquire));
        if (!construct)
        {
            construct = sig::Find(kSoundParamsCtor);
            if (!construct)
            {
                error = "sound_params_signature_mismatch";
                return false;
            }
        }
        if (!release)
        {
            release = sig::Find(kSoundParamsRelease);
            if (!release)
            {
                error = "sound_params_release_signature_mismatch";
                return false;
            }
        }
        std::uintptr_t play = reinterpret_cast<std::uintptr_t>(g_originalSoundRequest.load(std::memory_order_acquire));
        if (!play)
        {
            play = sig::Find(kSoundRequest);
            if (!play)
            {
                error = "sound_request_signature_mismatch";
                return false;
            }
        }
        const std::uint64_t holder = PedAudioHolder(pedEntity);
        if (!holder)
        {
            error = "no_ped_audio";
            return false;
        }
        // Older clips may carry another ped's sound at the next block's
        // offset: not played on this puppet.
        if (audioOffset >= kPedAudioSpan)
        {
            error = "neighbor_ped_audio";
            return false;
        }
        bool queued = false;
        std::uint32_t code = 0;
        if (!GuardedSoundRequest(play, construct, release, holder + audioOffset, soundRef,
                reinterpret_cast<std::uint64_t>(pedEntity), params, flags, queued, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        error = queued ? "queued" : "not_queued";
        return queued;
    }

    void CollectPlayRequestsSince(std::uint64_t& cursor, std::vector<SoundRequest>& out)
    {
        CollectSince(g_playRequests, kSoundRequestRing, g_nextPlayRequest, cursor, out);
    }
    namespace
    {

        bool GuardedDirectSound(std::uintptr_t play, std::uintptr_t construct, std::uintptr_t release, std::uint64_t audio,
            std::uint64_t soundRef, const std::uint8_t* recorded, std::uint64_t tracker, std::uint64_t envGroup, bool& queued,
            std::uint32_t& code)
        {
            __try
            {
                alignas(16) std::uint8_t params[kSoundParamsBytes + 0x40] = {};
                void* constructed = nullptr;
                if (!engine::Call(engine::kSoundParamsCtor, construct, constructed, params))
                    return engine::Failed(code);
                if (recorded)
                    CopyRecordedSoundParams(params, recorded);
                // The tracker is reference-counted by the core itself.
                std::memcpy(params + 0xC8, &tracker, sizeof(tracker));
                // The environment group, as the game's own weapon rattle
                // puts its holder's at +0xE0 before the
                // same public entry; the creator counts the use.
                if (envGroup)
                    std::memcpy(params + 0xE0, &envGroup, sizeof(envGroup));
                if (!engine::Call(engine::kCreateAndPlayPublic, play, queued, audio, soundRef, reinterpret_cast<std::uint64_t>(params)) ||
                    !engine::Call(engine::kSoundParamsRelease, release, params + 0x12C))
                    return engine::Failed(code);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    std::uint64_t WeaponAudio(void* weaponObject)
    {
        std::uint64_t weapon = 0;
        if (!weaponObject || !ReadU64(reinterpret_cast<std::uint64_t>(weaponObject) + 0x448, weapon))
            return 0;
        const std::uint64_t module = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
        if (weapon < 0x10000 || (weapon >= module && weapon < module + 0x7000000))
            return 0;
        std::uint64_t vtable = 0;
        if (!ReadU64(weapon, vtable) || vtable < module || vtable >= module + 0x7000000)
            return 0;
        return weapon;
    }
    namespace
    {

        // The static audio entities found in this game (sound_check_core.h
        // kImageAudioSignatures), as RVAs; one not found stays 0 and names
        // nothing. An object whose vtable has no signature takes its own,
        // read here once, when the vtable holds the class's method at the
        // slot.
        const census::ImageAudioTable& FoundImageAudio()
        {
            static const census::ImageAudioTable table = [] {
                census::ImageAudioTable found{};
                const std::uintptr_t base = sig::ImageBase();
                if (!base)
                    return found;
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
                const std::uintptr_t imageEnd = base + nt->OptionalHeader.SizeOfImage;
                for (std::size_t n = 0; n < census::kImageAudioCount; ++n)
                {
                    const census::ImageAudioSignatures& s = census::kImageAudioSignatures[n];
                    const std::uintptr_t object = sig::Find(*s.object);
                    std::uint64_t vtable = s.vtable ? sig::Find(*s.vtable) : 0;
                    if (!s.vtable && object)
                    {
                        const std::uintptr_t expected = s.method ? sig::Find(*s.method) : 0;
                        std::uint64_t live = 0;
                        std::uint64_t method = 0;
                        if (expected && ReadU64(object, live) && live > base && live < imageEnd &&
                            ReadU64(live + s.slot, method) && method == expected)
                            vtable = live;
                    }
                    if (object > base && vtable > base)
                        found.entry[n] = {static_cast<std::uint32_t>(object - base),
                            static_cast<std::uint32_t>(vtable - base)};
                }
                return found;
            }();
            return table;
        }
    }

    bool TrustedImageAudio(std::uint64_t address, std::string& status)
    {
        const std::uint64_t module = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
        std::uint32_t vtableRva = 0;
        char rva[24]{};
        std::snprintf(rva, sizeof(rva), "(0x%llx)", static_cast<unsigned long long>(address > module ? address - module : address));
        if (address <= module || !census::KnownImageAudio(address - module, vtableRva, FoundImageAudio()))
        {
            status = std::string("unknown_image_audio") + rva; // the RVA: evidence to extend kImageAudioSignatures
            return false;
        }
        std::uint64_t vtable = 0;
        if (!ReadU64(address, vtable) || vtable != module + vtableRva)
        {
            status = std::string("image_audio_vtable_mismatch") + rva;
            return false;
        }
        return true;
    }

    bool PlayDirectSound(void* pedEntity, std::uint64_t audioOffset, std::uint64_t global,
        std::uint64_t soundRef, const std::uint8_t* params, bool trackPed, std::string& error, std::uint64_t envGroup)
    {
        static std::uintptr_t play = 0;
        static std::uintptr_t release = 0;
        if (!play)
        {
            const std::uintptr_t create = sig::Find(kCreateAndPlayPublic), free = sig::Find(kSoundParamsRelease);
            if (!create || !free)
            {
                error = "direct_sound_signature_mismatch";
                return false;
            }
            play = create;
            release = free;
        }
        std::uintptr_t construct = reinterpret_cast<std::uintptr_t>(g_originalCtor.load(std::memory_order_acquire));
        if (!construct)
        {
            construct = sig::Find(kSoundParamsCtor);
            if (!construct)
            {
                error = "sound_params_signature_mismatch";
                return false;
            }
        }
        std::uint64_t audio = global;
        if (!audio)
        {
            const std::uint64_t holder = PedAudioHolder(pedEntity);
            if (!holder)
            {
                error = "no_ped_audio";
                return false;
            }
            if (audioOffset >= kPedAudioSpan)
            {
                error = "neighbor_ped_audio"; // another ped's sound in an older clip
                return false;
            }
            audio = holder + audioOffset;
        }
        const std::uint64_t tracker = trackPed ? PedSoundTracker(pedEntity) : 0;
        bool queued = false;
        std::uint32_t code = 0;
        if (!GuardedDirectSound(play, construct, release, audio, soundRef, params, tracker, envGroup, queued, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        error = queued ? "queued" : "not_queued";
        if (!queued)
        {
            // Why (the public entry and the core): the audio entity's state
            // (+0x14, 0 refuses) and its registration (+0x10, 0xFFFF refuses).
            std::uint32_t state = 0xFFFFFFFFu;
            std::uint16_t slot = 0;
            if (GuardedRead(&state, reinterpret_cast<const void*>(audio + 0x14), 4) &&
                GuardedRead(&slot, reinterpret_cast<const void*>(audio + 0x10), 2))
                error += state == 0 ? ":audio_state0" : slot == 0xFFFF ? ":audio_unregistered" : ":audio_ok";
        }
        return queued;
    }

    void CollectSoundStopsSince(std::uint64_t& cursor, std::vector<SoundStop>& out)
    {
        CollectSince(g_soundStops, kStopRing2, g_nextSoundStop, cursor, out);
    }

    std::uint64_t StaticCollisionAudio()
    {
        return sig::Find(kStaticCollisionAudio);
    }

        // Requested settings of a sound (the lookup StopAndForget inlines);
        // globals taken from its found bytes (imul ecx,[rip+d] at +0x1D and
        // mov rax,[rip+d] at +0x2B, past the hook patch). 0 when the sound has
        // none.
        std::uint64_t RequestedSettings(std::uint64_t sound)
        {
            static const std::uint64_t poolTable = sig::Find(kRequestedSettingsTable);
            static const std::uint64_t poolStride = sig::Find(kRequestedSettingsStride);
            if (!poolTable || !poolStride)
                return 0;
            std::uint16_t index = 0xFFFF;
            std::uint8_t bucket = 0;
            std::uint32_t stride = 0;
            std::uint64_t table = 0;
            std::uint64_t pool = 0;
            if (!GuardedRead(&index, reinterpret_cast<void*>(sound + 0xD6), 2) || index == 0xFFFF ||
                !GuardedRead(&bucket, reinterpret_cast<void*>(sound + 0xA0), 1) ||
                !GuardedRead(&stride, reinterpret_cast<void*>(poolStride), 4) ||
                !ReadU64(poolTable, table) || !table ||
                !ReadU64(table + static_cast<std::uint64_t>(bucket) * 0x2A860 + 0x2A850, pool) || !pool)
            {
                return 0;
            }
            return pool + static_cast<std::uint64_t>(index) * stride;
        }

        // The engine's own back reference of a live top-level sound, the
        // check before any port touches a sound the caller did not get from a
        // slot the engine nulls. The settings block is allocated from its
        // pool (which zeroes the block) and built with `mov [block], sound`;
        // the release frees the block (index free list outside the block) and
        // writes 0xFFFF to sound+0xD6. A top-level sound's vtable slot 1 is S38
        // (kSoundVariableSet). A freed or reused sound fails one of the three:
        // not a sound, index 0xFFFF or garbage, or the block names another
        // sound. Reads only, through the range filter (no fault, so no other
        // mod's vectored handler runs).
        std::atomic<std::uint64_t> g_linkChecks{0};
        std::atomic<std::uint64_t> g_linkNotSound{0};
        std::atomic<std::uint64_t> g_linkNoSettings{0};
        std::atomic<std::uint64_t> g_linkBroken{0};
        std::atomic<std::uint64_t> g_linkRefused[kSoundLinkPorts] = {};
        std::atomic<std::uint64_t> g_persistentLinkBroken{0};

        std::uint64_t LiveSettings(std::uint64_t sound)
        {
            g_linkChecks.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t vtable = 0;
            std::uint64_t slot1 = 0;
            static const std::uint64_t variableSet = sig::Find(kSoundVariableSet);
            if (!variableSet || !sound || (sound & 7u) != 0 ||
                !GuardedRead(&vtable, reinterpret_cast<const void*>(sound), 8) ||
                !GuardedRead(&slot1, reinterpret_cast<const void*>(vtable + 8), 8) || slot1 != variableSet)
            {
                g_linkNotSound.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            const std::uint64_t settings = RequestedSettings(sound);
            if (!settings)
            {
                g_linkNoSettings.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            std::uint64_t back = 0;
            if (!GuardedRead(&back, reinterpret_cast<const void*>(settings), 8) || back != sound ||
                !replay::mem::InUserRange(settings, 0x270, replay::mem::kGameHeapLowest))
            {
                g_linkBroken.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            return settings;
        }

        std::uint64_t PortSettings(std::uint64_t sound, SoundLinkPort port)
        {
            if (!sound)
                return 0;
            const std::uint64_t settings = LiveSettings(sound);
            if (!settings)
                g_linkRefused[port].fetch_add(1, std::memory_order_relaxed);
            return settings;
        }

        bool GuardedCall(void (*body)(void*), void* context, std::uint32_t& code)
        {
            __try
            {
                body(context);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

    bool ReadSoundVariable(std::uint64_t sound, std::uint32_t hash, float& value)
    {
        static std::uintptr_t get = 0;
        if (!get)
        {
            get = sig::Find(kGetVariable);
            if (!get)
                return false;
        }
        if (!sound || !PortSettings(sound, kLinkVariable))
            return false;
        bool ok = false;
        return engine::Call(engine::kGetVariable, get, ok, sound, hash, &value) && ok;
    }

    bool WriteSoundVariable(std::uint64_t sound, std::uint32_t hash, float value)
    {
        return std::strcmp(WriteSoundVariableWhy(sound, hash, value), "ok") == 0;
    }

    // Why S38 refuses (the lookup it runs, read with guarded reads once the
    // variable find and the root sound are found).
    const char* SoundVariableRefusal(std::uint64_t sound, std::uint32_t hash, float value)
    {
        if (!std::isfinite(value))
            return "bad_value";
        static const bool checked = sig::Find(kVariableFind) && sig::Find(kRootSound);
        if (!checked)
            return "unchecked";
        static const std::uint64_t tableAt = sig::Find(kSoundPoolTable);
        static const std::uint64_t strideAt = sig::Find(kSoundSlotStride);
        std::uint64_t table = 0;
        std::uint32_t stride = 0;
        if (!tableAt || !strideAt || !ReadU64(tableAt, table) || !table ||
            !GuardedRead(&stride, reinterpret_cast<const void*>(strideAt), 4) || !stride)
            return "no_pool";
        const auto poolOf = [&](std::uint64_t of, std::uint64_t& pool) {
            std::uint8_t bucket = 0;
            return GuardedRead(&bucket, reinterpret_cast<void*>(of + 0xA0), 1) &&
                ReadU64(table + static_cast<std::uint64_t>(bucket) * 0x2A860 + 0x2A848, pool) && pool;
        };
        std::uint64_t root = sound;
        for (int depth = 0; depth < 32; ++depth)
        {
            std::uint16_t parent = 0xFFFF;
            if (!GuardedRead(&parent, reinterpret_cast<void*>(root + 0xE2), 2))
                return "unreadable";
            if (parent == 0xFFFF)
                break;
            std::uint64_t pool = 0;
            if (!poolOf(root, pool))
                return "unreadable";
            root = pool + static_cast<std::uint64_t>(parent) * stride;
        }
        std::uint16_t block = 0xFFFF;
        if (!GuardedRead(&block, reinterpret_cast<void*>(root + 0x9C), 2))
            return "unreadable";
        if (block == 0xFFFF)
            return root == sound ? "no_block" : "no_block_on_parent";
        std::uint64_t pool = 0;
        if (!poolOf(sound, pool))
            return "unreadable";
        const std::uint64_t pairs = pool + static_cast<std::uint64_t>(block) * stride;
        std::int32_t count = 0;
        if (!GuardedRead(&count, reinterpret_cast<void*>(pairs + 0x80), 4) || count < 0 || count > 16)
            return "bad_block";
        for (std::int32_t n = 0; n < count; ++n)
        {
            std::uint32_t key = 0;
            if (!GuardedRead(&key, reinterpret_cast<void*>(pairs + n * 8), 4))
                return "unreadable";
            if (key == hash)
                return "found";
        }
        return count ? "absent" : "empty_block";
    }

    const char* WriteSoundVariableWhy(std::uint64_t sound, std::uint32_t hash, float value)
    {
        if (!sound)
            return "no_sound";
        if (!RequestedSettings(sound))
            return "no_settings";
        if (!PortSettings(sound, kLinkVariable))
            return "sound_gone";
        struct Context { std::uint64_t sound; std::uint32_t hash; float value; bool ok; };
        Context context{sound, hash, value, false};
        std::uint32_t code = 0;
        if (!GuardedCall([](void* raw) {
                auto* c = static_cast<Context*>(raw);
                auto* vtable = *reinterpret_cast<SetVariableFn**>(c->sound);
                c->ok = vtable[1](c->sound, &c->hash, c->value);
            }, &context, code))
            return "exception";
        if (context.ok)
            return "ok";
        const char* why = SoundVariableRefusal(sound, hash, value);
        return std::strcmp(why, "found") == 0 ? "refused_found" : why;
    }

    bool SetSoundPosition(std::uint64_t sound, const float position[4])
    {
        static std::uintptr_t set = 0;
        // With SoundFxProbe=1 the read probe hooked this port: the mod calls
        // the probe's trampoline, which is the engine's function, uncounted.
        if (!set)
        {
            const std::uintptr_t at = sig::Find(kSetPosition);
            if (!at)
                return false;
            void* probed = replay::soundfx::OriginalFor(at - sig::ImageBase());
            set = probed ? reinterpret_cast<std::uintptr_t>(probed) : at;
        }
        const std::uint64_t settings = PortSettings(sound, kLinkAdjust);
        if (!settings)
            return false;
        alignas(16) float vector[4] = {position[0], position[1], position[2], 0.0f};
        return engine::Call(engine::kSetPosition, set, settings, vector);
    }

    bool StartPersistentSound(std::uint64_t audio, std::uint64_t soundRef, const std::uint8_t* params,
        std::uint64_t* slot, std::string& error)
    {
        static std::uintptr_t play = 0;
        static std::uintptr_t release = 0;
        if (!play)
        {
            // A port the read probe hooked (SoundFxProbe=1) is called through
            // the probe's trampoline (PrepareAndPlayPort).
            void* port = PrepareAndPlayPort();
            const std::uintptr_t free = sig::Find(kSoundParamsRelease);
            if (!port || !free)
            {
                error = "persistent_signature_mismatch";
                return false;
            }
            play = reinterpret_cast<std::uintptr_t>(port);
            release = free;
        }
        const auto create = reinterpret_cast<std::uintptr_t>(g_originalPersistentCreate.load(std::memory_order_acquire));
        const auto construct = reinterpret_cast<std::uintptr_t>(g_originalCtor.load(std::memory_order_acquire));
        if (!create || !construct)
        {
            error = "observer_not_installed";
            return false;
        }
        StopPersistentSound(slot);
        struct Context
        {
            std::uintptr_t create; std::uintptr_t construct; std::uintptr_t release; std::uintptr_t play;
            std::uint64_t audio; std::uint64_t soundRef; const std::uint8_t* recorded; std::uint64_t* slot;
        };
        Context context{create, construct, release, play, audio, soundRef, params, slot};
        std::uint32_t code = 0;
        const bool ok = GuardedCall([](void* raw) {
            auto* c = static_cast<Context*>(raw);
            alignas(16) std::uint8_t params[kSoundParamsBytes + 0x40] = {};
            // A faulted call raises again: GuardedCall reports its code.
            void* constructed = nullptr;
            if (!engine::Call(engine::kSoundParamsCtor, c->construct, constructed, params))
                engine::Raise();
            if (c->recorded)
                CopyRecordedSoundParams(params, c->recorded);
            // Descriptor: the metadata ref at +0xC (0x14 bytes).
            alignas(16) std::uint8_t descriptor[0x20] = {};
            std::memcpy(descriptor + 0xC, &c->soundRef, sizeof(c->soundRef));
            std::uint64_t created = 0;
            if (!engine::Call(engine::kPersistentCreate, c->create, created, c->audio,
                    reinterpret_cast<std::uint64_t>(descriptor), reinterpret_cast<std::uint64_t>(c->slot),
                    reinterpret_cast<std::uint64_t>(params)) ||
                !engine::Call(engine::kSoundParamsRelease, c->release, params + 0x12C))
                engine::Raise();
            if (*c->slot && !engine::Call(engine::kPrepareAndPlay, c->play, *c->slot, 0, 0, 0, 0))
                engine::Raise();
        }, &context, code);
        if (!ok)
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        error = *slot ? "playing" : "not_created";
        return *slot != 0;
    }

    void CollectEntitySoundsSince(std::uint64_t& cursor, std::vector<EntitySoundCreate>& out)
    {
        CollectSince(g_entitySounds, kEntitySoundRing, g_nextEntitySound, cursor, out);
    }

    bool StartEntitySound(std::uint64_t audio, std::uint64_t soundRef, bool byHash, const std::uint8_t* params,
        std::uint64_t* slot, std::string& error)
    {
        static std::uintptr_t play = 0;
        static std::uintptr_t release = 0;
        if (!play)
        {
            // A port the read probe hooked (SoundFxProbe=1) is called through
            // the probe's trampoline (PrepareAndPlayPort).
            void* port = PrepareAndPlayPort();
            const std::uintptr_t free = sig::Find(kSoundParamsRelease);
            if (!port || !free)
            {
                error = "persistent_signature_mismatch";
                return false;
            }
            play = reinterpret_cast<std::uintptr_t>(port);
            release = free;
        }
        // Through the trampolines: the replayed create is not recorded again.
        const auto byRef = reinterpret_cast<std::uintptr_t>(g_originalByRef.load(std::memory_order_acquire));
        const auto byHashFn = reinterpret_cast<std::uintptr_t>(g_originalByHash.load(std::memory_order_acquire));
        const auto construct = reinterpret_cast<std::uintptr_t>(g_originalCtor.load(std::memory_order_acquire));
        if (!construct || (byHash ? !byHashFn : !byRef) || !audio)
        {
            error = "observer_not_installed";
            return false;
        }
        // `audio` comes from a clip (an image offset or an offset in the
        // puppet). An audEntity is polymorphic: its first field is a vtable in
        // the image whose first slot is code in the image. Anything else is
        // not called on.
        {
            std::uint64_t vtable = 0, first = 0;
            if ((audio & 7u) != 0 || !GuardedRead(&vtable, reinterpret_cast<const void*>(audio), sizeof(vtable)) ||
                vtable < g_base || vtable >= g_imageEnd || (vtable & 7u) != 0 ||
                !GuardedRead(&first, reinterpret_cast<const void*>(vtable), sizeof(first)) || first < g_base ||
                first >= g_imageEnd)
            {
                error = "audio_entity_not_an_object";
                return false;
            }
        }
        StopPersistentSound(slot);
        struct Context
        {
            std::uintptr_t byRef; std::uintptr_t byHash; bool useHash; std::uintptr_t construct; std::uintptr_t release;
            std::uintptr_t play; std::uint64_t audio; std::uint64_t soundRef; const std::uint8_t* recorded;
            std::uint64_t* slot;
        };
        Context context{byRef, byHashFn, byHash, construct, release, play, audio, soundRef, params, slot};
        std::uint32_t code = 0;
        const bool ok = GuardedCall([](void* raw) {
            auto* c = static_cast<Context*>(raw);
            alignas(16) std::uint8_t params[kSoundParamsBytes + 0x40] = {};
            // A faulted call raises again: GuardedCall reports its code.
            void* constructed = nullptr;
            if (!engine::Call(engine::kSoundParamsCtor, c->construct, constructed, params))
                engine::Raise();
            if (c->recorded)
                CopyRecordedSoundParams(params, c->recorded); // tracker [0xC8, 0xE8) left clear
            const bool called = c->useHash
                ? engine::Call(engine::kPersistentByHash, c->byHash, c->audio, static_cast<std::uint32_t>(c->soundRef),
                      reinterpret_cast<std::uint64_t>(c->slot), reinterpret_cast<std::uint64_t>(params))
                : engine::Call(engine::kPersistentByRef, c->byRef, c->audio, c->soundRef, reinterpret_cast<std::uint64_t>(c->slot),
                      reinterpret_cast<std::uint64_t>(params));
            if (!called || !engine::Call(engine::kSoundParamsRelease, c->release, params + 0x12C))
                engine::Raise();
            if (*c->slot && !engine::Call(engine::kPrepareAndPlay, c->play, *c->slot, 0, 0, 0, 0))
                engine::Raise();
        }, &context, code);
        if (!ok)
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        error = *slot ? "playing" : "not_created";
        return *slot != 0;
    }

    void StopPersistentSound(std::uint64_t* slot)
    {
        if (!slot || !*slot || !RequestedSettings(*slot))
        {
            if (slot)
                *slot = 0;
            return;
        }
        // The slot is the engine's weak reference (it nulls it when the sound
        // ends), so the stop goes ahead; a broken back reference here is
        // counted only (expected 0).
        if (!LiveSettings(*slot))
            g_persistentLinkBroken.fetch_add(1, std::memory_order_relaxed);
        const auto stop = reinterpret_cast<std::uintptr_t>(g_originalStopAndForget.load(std::memory_order_acquire));
        if (!stop)
            return;
        engine::Call(engine::kStopAndForget, stop, *slot, 0);
        *slot = 0;
    }

    void CollectImpactsSince(std::uint64_t& cursor, std::vector<ImpactCall>& out)
    {
        CollectSince(g_impacts, kImpactRing, g_nextImpact, cursor, out);
    }

    void CollectMeleeSince(std::uint64_t& cursor, std::vector<MeleeCall>& out)
    {
        CollectSince(g_melee, kMeleeRing, g_nextMelee, cursor, out);
    }

    std::uint64_t MeleeAudio(void* pedEntity)
    {
        const std::uint64_t holder = PedAudioHolder(pedEntity);
        return holder ? holder + 0x1B60 : 0;
    }
    namespace
    {

        bool GuardedMelee(std::uintptr_t play, void* melee, void* info, std::uint32_t& code)
        {
            return engine::Call(engine::kMeleeImpact, play, melee, info) || engine::Failed(code);
        }

        // The action result lookup (see kActionResultSite): the site found
        // with the manager displacement masked, then its call target with its
        // prologue.
        std::uintptr_t g_actionResultLookup = 0;
        std::uint8_t* g_actionResultManager = nullptr;
        bool g_actionResultChecked = false;
        std::string g_actionResultCheckError;

        bool EnsureActionResultLookup(std::string& error)
        {
            if (g_actionResultChecked)
            {
                error = g_actionResultCheckError;
                return g_actionResultLookup != 0;
            }
            g_actionResultChecked = true;
            const std::uintptr_t manager = sig::Find(kActionResultManager);
            if (!sig::Find(kActionResultSite) || !manager)
            {
                g_actionResultCheckError = error = "action_result_site_mismatch";
                return false;
            }
            const std::uintptr_t lookup = sig::Find(kActionResultLookup);
            if (!lookup)
            {
                g_actionResultCheckError = error = "action_result_lookup_mismatch";
                return false;
            }
            g_actionResultManager = reinterpret_cast<std::uint8_t*>(manager);
            g_actionResultLookup = lookup;
            return true;
        }

        bool GuardedActionResultLookup(void* manager, std::uint32_t hash, void*& result, std::uint32_t& code)
        {
            return engine::Call(engine::kActionResultLookup, g_actionResultLookup, result, manager, hash) ||
                engine::Failed(code);
        }

        // The engine's action result for a recorded name hash (0 when the
        // lookup is unavailable, the manager is not built or the hash is
        // not in its map). The result must carry the hash at +0x48.
        std::uint64_t FindActionResult(std::uint32_t hash, std::string& status)
        {
            if (!hash)
            {
                status = "no_hash";
                return 0;
            }
            if (!EnsureActionResultLookup(status))
                return 0;
            std::uint64_t manager = 0;
            if (!ReadU64(reinterpret_cast<std::uint64_t>(g_actionResultManager), manager) || !manager)
            {
                status = "no_manager";
                return 0;
            }
            void* found = nullptr;
            std::uint32_t code = 0;
            if (!GuardedActionResultLookup(reinterpret_cast<void*>(manager), hash, found, code))
            {
                char text[40]{};
                std::snprintf(text, sizeof(text), "lookup_exception_%08X", code);
                status = text;
                return 0;
            }
            const std::uint64_t result = reinterpret_cast<std::uint64_t>(found);
            std::uint64_t vtable = 0;
            std::uint32_t foundHash = 0;
            if (!result)
            {
                status = "not_found";
                return 0;
            }
            if (!ReadU64(result, vtable) || !vtable || !ReadBytes(result + 0x48, &foundHash, sizeof(foundHash)) ||
                foundHash != hash)
            {
                status = "hash_mismatch";
                return 0;
            }
            status.clear();
            return result;
        }
    }

    bool PlayMeleeImpact(void* pedEntity, std::uint64_t info, std::uint64_t infoVtable,
        std::uint32_t infoHash, std::string& error)
    {
        auto play = reinterpret_cast<std::uintptr_t>(g_originalMeleeImpact.load(std::memory_order_acquire));
        if (!play)
        {
            play = sig::Find(kAudioMeleeImpact);
            if (!play)
            {
                error = "melee_signature_mismatch";
                return false;
            }
        }
        // The entry reads the ped through the melee audio (+0x18) without a
        // null check: validate the object before calling.
        const std::uint64_t holder = PedAudioHolder(pedEntity);
        const std::uint64_t melee = holder ? holder + 0x1B60 : 0;
        std::uint64_t owner = 0;
        std::uint64_t ped = 0;
        std::uint64_t audioEntity = 0;
        if (!melee || !ReadU64(melee + 0x10, owner) || owner != holder ||
            !ReadU64(melee + 0x18, ped) || ped != reinterpret_cast<std::uint64_t>(pedEntity) ||
            !ReadU64(melee + 8, audioEntity) || !audioEntity)
        {
            error = "melee_audio_not_ready";
            return false;
        }
        std::uint64_t liveVtable = 0;
        std::uint32_t liveHash = 0;
        void* definition = reinterpret_cast<void*>(info);
        if (!info || !ReadU64(info, liveVtable) || liveVtable != infoVtable ||
            !ReadBytes(info + 0x48, &liveHash, sizeof(liveHash)) || liveHash != infoHash)
        {
            // A clip from disk has no pointer (and one from memory can outlive
            // its object): the engine's own action result for the recorded
            // hash, with its material set, as TASK_MELEE and the clone tasks
            // find it, or nothing.
            std::string lookupStatus;
            const std::uint64_t found = FindActionResult(infoHash, lookupStatus);
            if (!found)
            {
                // No object rebuilt from the recorded vtable: that number
                // comes from the clip and the entry would take it as the
                // action result's class. The hit is refused, logged by the
                // caller (POSE_V2_PLAYBACK_MELEE status).
                error = "refused_no_action_result(" + lookupStatus + ")";
                return false;
            }
            definition = reinterpret_cast<void*>(found);
            error = "lookup:";
        }
        else
        {
            error = "definition:";
        }
        std::uint32_t code = 0;
        if (!GuardedMelee(play, reinterpret_cast<void*>(melee), definition, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error += text;
            return false;
        }
        error += "played";
        return true;
    }

    void CollectExplosionsSince(std::uint64_t& cursor, std::vector<ExplosionAudioCall>& out)
    {
        CollectSince(g_explosions, kExplosionRing, g_nextExplosion, cursor, out);
    }

    void CollectAnimBatchesSince(std::uint64_t& cursor, std::vector<AnimAudioBatch>& out)
    {
        CollectSince(g_animBatches, kAnimRing, g_nextAnimBatch, cursor, out);
    }
    namespace
    {

        bool GuardedExplosionPlay(std::uintptr_t play, void* manager, std::uint32_t ref, const float* position,
            const ExplosionAudioCall& call, std::uint32_t& code)
        {
            return engine::Call(engine::kExplosionPlay, play, manager, ref, position, call.underwater, call.b31, call.interior,
                       call.b32) ||
                engine::Failed(code);
        }

        bool GuardedTagRef(std::uintptr_t fn, std::int32_t tag, std::uint32_t& ref, std::uint32_t& code)
        {
            std::uint32_t out = 0;
            std::uint32_t* got = nullptr;
            if (!engine::Call(engine::kExplosionTagRef, fn, got, &out, tag))
                return engine::Failed(code);
            if (GuardedRead(&ref, got, sizeof(ref)))
                return true;
            code = EXCEPTION_ACCESS_VIOLATION; // the returned ref unreadable
            return false;
        }
    }

    bool PlayExplosionAudio(const ExplosionAudioCall& call, std::string& error)
    {
        static const std::uintptr_t explosionPlay = sig::Find(kExplosionPlay);
        static const std::uintptr_t tagRef = sig::Find(kExplosionTagRef);
        static const std::uintptr_t manager = sig::Find(kExplosionManager);
        if (!explosionPlay || !tagRef || !manager)
        {
            error = "explosion_signature_mismatch";
            return false;
        }
        const auto valid = [](std::uint32_t ref) { return ref != 0 && ref != 0xFFFFFFFFu; };
        std::uint32_t ref = call.hasInstance && valid(call.overrideRef) ? call.overrideRef : 0;
        std::uint32_t code = 0;
        if (!ref)
        {
            if (!call.hasInstance || call.tag < 0 || call.tag >= 0x24)
            {
                error = "no_tag";
                return false;
            }
            if (!GuardedTagRef(tagRef, call.tag, ref, code))
            {
                char text[40]{};
                std::snprintf(text, sizeof(text), "tag_ref_exception_%08X", code);
                error = text;
                return false;
            }
            if (!valid(ref))
            {
                error = "tag_ref_unknown";
                return false;
            }
        }
        alignas(16) float position[4] = {call.position[0], call.position[1], call.position[2], 0.0f};
        t_playQueued = -1;
        t_playQueueBefore = -1;
        t_playingExplosion = true;
        const bool ok = GuardedExplosionPlay(explosionPlay, reinterpret_cast<std::uint8_t*>(manager), ref, position, call, code);
        t_playingExplosion = false;
        if (!ok)
        {
            char text[40]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        // The call returning is not the sound playing: the manager queue
        // (16 requests a frame) may refuse it.
        if (t_playQueued == 0)
        {
            error = t_playQueueBefore >= 16 ? "queue_full" : "not_queued";
            return false;
        }
        error = t_playQueued == 1 ? "accepted" : "played_unseen";
        return true;
    }

    void SetSilencedFootAudios(const std::vector<std::uint64_t>& feet)
    {
        const std::uint32_t count = static_cast<std::uint32_t>((std::min)(feet.size(), kMaxSilencedFeet));
        // Shrink first, fill, then publish the new count (as the fx owners).
        if (count < g_silencedFeetCount.load(std::memory_order_relaxed))
            g_silencedFeetCount.store(count, std::memory_order_release);
        for (std::uint32_t n = 0; n < count; ++n)
            g_silencedFeet[n].store(feet[n], std::memory_order_relaxed);
        g_silencedFeetCount.store(count, std::memory_order_release);
    }

    std::uint64_t SilencedFootsteps()
    {
        return g_silencedFootsteps.load(std::memory_order_relaxed);
    }

    void SetSilencedPuppetAudios(const std::vector<std::uint64_t>& holders)
    {
        SetSilencedPuppetAudios(holders, {});
    }

    // Each holder's clip entity is stored before the holder (a detour that
    // finds the holder reads an owner of this list or of the previous one at
    // the same index, the same puppet in practice).
    void SetSilencedPuppetAudios(const std::vector<std::uint64_t>& holders, const std::vector<std::uint32_t>& owners)
    {
        const std::uint32_t count = static_cast<std::uint32_t>((std::min)(holders.size(), kMaxSilencedPuppetAudios));
        if (count < g_silencedPuppetAudioCount.load(std::memory_order_relaxed))
            g_silencedPuppetAudioCount.store(count, std::memory_order_release);
        for (std::uint32_t n = 0; n < count; ++n)
        {
            g_silencedPuppetOwner[n].store(n < owners.size() ? owners[n] : replay::puppetaudio::kNoOwner,
                std::memory_order_relaxed);
            g_silencedPuppetAudio[n].store(holders[n], std::memory_order_release);
        }
        g_silencedPuppetAudioCount.store(count, std::memory_order_release);
    }

    void SetSilencedPuppetWeaponAudios(const std::vector<std::uint64_t>& weapons)
    {
        const std::uint32_t count = static_cast<std::uint32_t>((std::min)(weapons.size(), kMaxSilencedPuppetWeapons));
        if (count < g_silencedPuppetWeaponCount.load(std::memory_order_relaxed))
            g_silencedPuppetWeaponCount.store(count, std::memory_order_release);
        for (std::uint32_t n = 0; n < count; ++n)
            g_silencedPuppetWeapon[n].store(weapons[n], std::memory_order_relaxed);
        g_silencedPuppetWeaponCount.store(count, std::memory_order_release);
    }

    std::uint64_t RefusedPuppetWeaponOneShots()
    {
        return g_refusedPuppetWeaponOneShots.load(std::memory_order_relaxed);
    }

    void SetRefusePuppetOwnPersistent(bool enabled)
    {
        g_refuseOwnPersistent.store(enabled, std::memory_order_relaxed);
    }

    bool RefusePuppetOwnPersistent()
    {
        return g_refuseOwnPersistent.load(std::memory_order_relaxed);
    }

    void SetOwnPersistentCallers(const std::vector<std::uint32_t>& callers)
    {
        const std::uint32_t count = static_cast<std::uint32_t>((std::min)(callers.size(), kMaxOwnPersistentCallers));
        if (count < g_ownPersistentCallerCount.load(std::memory_order_relaxed))
            g_ownPersistentCallerCount.store(count, std::memory_order_release);
        for (std::uint32_t n = 0; n < count; ++n)
            g_ownPersistentCaller[n].store(callers[n], std::memory_order_relaxed);
        g_ownPersistentCallerCount.store(count, std::memory_order_release);
    }

    // Sorted and without repeats for the detour's binary search; the list is
    // hidden (count 0) while it is rewritten.
    std::size_t SetOwnPersistentKeys(const std::vector<std::uint64_t>& keys)
    {
        std::vector<std::uint64_t> sorted(keys);
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        const std::uint32_t count = static_cast<std::uint32_t>((std::min)(sorted.size(), kMaxOwnPersistentKeys));
        g_ownPersistentKeyCount.store(0, std::memory_order_release);
        for (std::uint32_t n = 0; n < count; ++n)
            g_ownPersistentKey[n].store(sorted[n], std::memory_order_relaxed);
        g_ownPersistentKeyCount.store(count, std::memory_order_release);
        return count;
    }

    // Packets mode at the ports.
    void SetBlockPuppetSounds(bool enabled) { g_blockPuppetSounds.store(enabled, std::memory_order_release); }
    void SetBlockPacketLives(const replay::soundowner::PacketLives* lives, bool blockAll)
    {
        g_blockAll.store(blockAll, std::memory_order_relaxed);
        g_blockLives.store(lives, std::memory_order_release);
    }
    void SetBlockPlayheadMs(std::uint32_t playheadMs) { g_blockPlayheadMs.store(playheadMs, std::memory_order_relaxed); }
    bool BlockPuppetSounds() { return g_blockPuppetSounds.load(std::memory_order_acquire); }
    std::uint64_t RefusedUnmadeSets() { return g_refusedUnmadeSets.load(std::memory_order_relaxed); }
    void BeginUnrecordedPersistent()
    {
        g_unrecordedRefused.store(0, std::memory_order_relaxed);
        g_unrecordedKeptByEsnd.store(0, std::memory_order_relaxed);
        g_unrecordedSampleCount.store(0, std::memory_order_relaxed);
    }
    std::string UnrecordedPersistentLine()
    {
        std::string samples;
        const std::uint32_t n = g_unrecordedSampleCount.load(std::memory_order_relaxed);
        char part[64]{};
        for (std::uint32_t k = 0; k < n && k < kUnrecordedSamples; ++k)
        {
            const std::uint32_t owner = g_unrecordedOwner[k].load(std::memory_order_relaxed);
            std::snprintf(part, sizeof(part), "%s%u:%u@0x%llx/0x%x", samples.empty() ? "" : "|", owner >> 16, owner & 0xFFFFu,
                static_cast<unsigned long long>(g_unrecordedRef[k].load(std::memory_order_relaxed)),
                g_unrecordedSite[k].load(std::memory_order_relaxed));
            samples += part;
        }
        char line[160]{};
        std::snprintf(line, sizeof(line), "PUPPET_UNRECORDED_PERSISTENT,refused=%llu,kept_by_esnd=%llu,samples=",
            static_cast<unsigned long long>(g_unrecordedRefused.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_unrecordedKeptByEsnd.load(std::memory_order_relaxed)));
        return std::string(line) + (samples.empty() ? "none" : samples);
    }
    // vehicle_sound_rule.h.
    void SetSilencedVehicleAudios(const std::vector<std::uint64_t>& audios, const std::vector<std::uint32_t>& owners)
    {
        const std::uint32_t count = static_cast<std::uint32_t>((std::min)(audios.size(), kMaxSilencedVehicles));
        if (count < g_silencedVehicleCount.load(std::memory_order_relaxed))
            g_silencedVehicleCount.store(count, std::memory_order_release);
        for (std::uint32_t n = 0; n < count; ++n)
        {
            g_silencedVehicleOwner[n].store(n < owners.size() ? owners[n] : replay::puppetaudio::kNoOwner,
                std::memory_order_relaxed);
            g_silencedVehicleAudio[n].store(audios[n], std::memory_order_release);
        }
        g_silencedVehicleCount.store(count, std::memory_order_release);
    }
    void SetVehicleOwnBlock(bool enabled) { g_vehicleOwnBlock.store(enabled, std::memory_order_relaxed); }
    bool VehicleOwnBlock() { return g_vehicleOwnBlock.load(std::memory_order_relaxed); }
    std::uint64_t VehicleOwnRefused() { return g_vehicleOwnRefused.load(std::memory_order_relaxed); }
    std::uint64_t VehicleOwnPassed() { return g_vehicleOwnPassed.load(std::memory_order_relaxed); }

    // Any thread (the census creators, before the original). Only an own
    // create of a listed vehicle is judged and noted (port 1 creator A, 2
    // creator B); the replay's own (an emit scope, a replayed system call)
    // and a create without a persistent reference (the game's queue drains
    // a request with none: the generic player's mode c births, a track's
    // create-and-play) never reach the rule. The vehicle's own one-shots
    // come with one (the create-and-play wrapper passes its caller's slot
    // through to the creator's wrapper, which zeroes it when the creator
    // returns 0; the wrapper then plays nothing).
    bool RefuseVehicleOwnCreate(std::uint8_t core, std::uint64_t audio, std::uint64_t soundRef, std::uint64_t out,
        std::uint32_t caller)
    {
        if (g_silencedVehicleCount.load(std::memory_order_relaxed) == 0 || !g_vehicleOwnBlock.load(std::memory_order_relaxed) ||
            !g_blockPuppetSounds.load(std::memory_order_relaxed))
            return false;
        std::uint32_t owner = replay::puppetaudio::kNoOwner;
        if (!SilencedVehicleAudioOwner(audio, owner))
            return false;
        const bool replayMade = t_replayEmitDepth > 0 || t_replayedSystemDepth > 0;
        const bool queueBirth = !out;
        const replay::soundowner::PacketLives* lives = g_blockLives.load(std::memory_order_acquire);
        if (replayMade || queueBirth || !lives)
            return false;
        const replay::soundowner::LifeVerdict verdict =
            lives->Judge(owner, soundRef, g_blockPlayheadMs.load(std::memory_order_relaxed));
        const bool refuse = replay::vehiclesound::RefuseVehicleOwnCreate(true, true, true, replayMade, queueBirth, true, verdict,
            g_blockAll.load(std::memory_order_relaxed));
        NoteBlocked(static_cast<std::uint8_t>(core + 1), owner, soundRef, caller, verdict, refuse);
        (refuse ? g_vehicleOwnRefused : g_vehicleOwnPassed).fetch_add(1, std::memory_order_relaxed);
        return refuse;
    }

    std::uint64_t BlockedPuppetCreates() { return g_blockedPuppetCreates.load(std::memory_order_relaxed); }
    void CollectBlockedPuppetCreatesSince(std::uint64_t& cursor, std::vector<BlockedPuppetCreate>& out)
    {
        CollectSince(g_blocked, kBlockedRing, g_nextBlocked, cursor, out);
    }

    std::uint64_t RefusedPuppetOwnPersistent()
    {
        return g_refusedOwnPersistent.load(std::memory_order_relaxed) -
            g_refusedOwnPersistentBase.load(std::memory_order_relaxed);
    }

    std::uint64_t KeptPuppetOwnPersistent()
    {
        return g_keptOwnPersistent.load(std::memory_order_relaxed) - g_keptOwnPersistentBase.load(std::memory_order_relaxed);
    }

    void BeginPuppetOwnPersistent()
    {
        g_refusedOwnPersistentBase.store(g_refusedOwnPersistent.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_keptOwnPersistentBase.store(g_keptOwnPersistent.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    std::string PuppetOwnPersistentSamples()
    {
        // ref@caller/frame1/frame2#owner ("s:i" of the clip entity, "hidden"
        // for the hidden player and his mount).
        const auto list = [](const std::atomic<std::uint64_t>* refs, const std::atomic<std::uint32_t>* callers,
                              const std::atomic<std::uint32_t>* owners, std::atomic<std::uint32_t> (*frames)[2],
                              std::uint64_t total) {
            std::string text;
            for (std::uint64_t n = 0; n < total && n < kOwnPersistentSamples; ++n)
            {
                const std::uint32_t owner = owners[n].load(std::memory_order_relaxed);
                char who[24]{};
                if (owner == replay::puppetaudio::kNoOwner)
                    std::snprintf(who, sizeof(who), "hidden");
                else
                    std::snprintf(who, sizeof(who), "%u:%u", owner >> 16, owner & 0xFFFFu);
                char item[112]{};
                std::snprintf(item, sizeof(item), "%s0x%llx@0x%X/0x%X/0x%X#%s", text.empty() ? "" : "|",
                    static_cast<unsigned long long>(refs[n].load(std::memory_order_relaxed)),
                    static_cast<unsigned>(callers[n].load(std::memory_order_relaxed)),
                    static_cast<unsigned>(frames[n][0].load(std::memory_order_relaxed)),
                    static_cast<unsigned>(frames[n][1].load(std::memory_order_relaxed)), who);
                text += item;
            }
            return text.empty() ? std::string("none") : text;
        };
        return "refused:" +
            list(g_refusedPersistentRef, g_refusedPersistentCaller, g_refusedPersistentOwner, g_refusedPersistentFrame,
                RefusedPuppetOwnPersistent()) +
            ";kept:" +
            list(g_keptPersistentRef, g_keptPersistentCaller, g_keptPersistentOwner, g_keptPersistentFrame,
                KeptPuppetOwnPersistent());
    }

    std::uint64_t RefusedPuppetOneShots()
    {
        return g_refusedPuppetOneShots.load(std::memory_order_relaxed);
    }

    std::uint64_t BeginRefusedPuppetOneShots()
    {
        const std::uint64_t total = g_refusedPuppetOneShots.load(std::memory_order_relaxed);
        g_refusedOneShotBase.store(total, std::memory_order_relaxed);
        return total;
    }

    std::string RefusedPuppetOneShotSamples()
    {
        std::string text;
        const std::uint64_t since = g_refusedOneShotBase.load(std::memory_order_relaxed);
        const std::uint64_t total = g_refusedPuppetOneShots.load(std::memory_order_relaxed);
        for (std::uint64_t n = 0; since + n < total && n < kRefusedOneShotSamples; ++n)
        {
            char item[48]{};
            std::snprintf(item, sizeof(item), "%s0x%llx@0x%X", text.empty() ? "" : "|",
                static_cast<unsigned long long>(g_refusedOneShotRef[n].load(std::memory_order_relaxed)),
                static_cast<unsigned>(g_refusedOneShotCaller[n].load(std::memory_order_relaxed)));
            text += item;
        }
        return text.empty() ? "none" : text;
    }

    ReplayEmitScope::ReplayEmitScope() { ++t_replayEmitDepth; }
    ReplayEmitScope::~ReplayEmitScope() { --t_replayEmitDepth; }
    bool ReplayEmittingHere() { return t_replayEmitDepth > 0; }
    bool IsPuppetSpeechEntity(std::uint64_t speechEntity)
    {
        std::uint32_t owner = 0;
        return PuppetSpeechOwner(speechEntity, owner);
    }
    namespace
    {

        bool GuardedAddAnimEvent(std::uintptr_t add, void* audio, const std::uint8_t* event, std::uint32_t& code)
        {
            return engine::Call(engine::kAddAnimEvent, add, audio, event) || engine::Failed(code);
        }
    }

    std::int32_t AnimEventCount(std::uint64_t audio)
    {
        std::int32_t count = -1;
        if (!audio || !GuardedRead(&count, reinterpret_cast<const void*>(audio + 0xD8), 4))
            return -1;
        return count;
    }

    bool QueueAnimEvent(void* pedEntity, std::uint32_t hash, std::int32_t extra, std::uint64_t& audioOut,
        std::string& status)
    {
        audioOut = 0;
        // Found once: not found disables the replay of these events.
        static const std::uintptr_t addAnimEvent = sig::Find(kAddAnimEvent);
        if (!addAnimEvent)
        {
            status = "signature_mismatch";
            return false;
        }
        const std::uint64_t audio = PedAudioHolder(pedEntity);
        if (!audio)
        {
            status = "no_audio";
            return false;
        }
        std::uint64_t vtable = 0;
        if (!ReadU64(audio, vtable))
        {
            status = "no_audio";
            return false;
        }
        bool pedVtable = false;
        for (const sig::Signature* pedAudio : replay::entityquery::kHolderVtables)
        {
            const std::uintptr_t known = sig::Find(*pedAudio);
            pedVtable = pedVtable || (known && vtable == known);
        }
        if (!pedVtable)
        {
            status = "vtable_other";
            return false;
        }
        std::uint64_t slot = 0;
        if (!ReadU64(vtable + 0xB8, slot) || slot != addAnimEvent)
        {
            status = "slot_other";
            return false;
        }
        std::uint64_t owner = 0;
        if (!ReadU64(audio + 0x4600, owner) || owner != reinterpret_cast<std::uint64_t>(pedEntity))
        {
            status = "owner_other";
            return false;
        }
        std::uint32_t active = 0;
        std::int32_t before = -1;
        if (!GuardedRead(&active, reinterpret_cast<const void*>(audio + 0x14), 4) ||
            !GuardedRead(&before, reinterpret_cast<const void*>(audio + 0xD8), 4) || before < 0 || before > 4)
        {
            status = "no_audio";
            return false;
        }
        audioOut = audio;
        if (!active)
        {
            status = "inactive";
            return false;
        }
        if (before >= 4)
        {
            status = "queue_full";
            return false;
        }
        for (std::int32_t n = 0; n < before; ++n)
        {
            std::int32_t queuedExtra = 0;
            std::uint32_t queuedHash = 0;
            if (GuardedRead(&queuedExtra, reinterpret_cast<const void*>(audio + 0x78 + n * 0x18), 4) &&
                GuardedRead(&queuedHash, reinterpret_cast<const void*>(audio + 0x78 + n * 0x18 + 4), 4) &&
                queuedExtra == extra && queuedHash == hash)
            {
                status = "duplicate";
                return false;
            }
        }
        // The MoVE callback's event: {+0 extra, +4 hash, +8 word 0, +0x10 qword 0}.
        alignas(8) std::uint8_t event[0x18] = {};
        std::memcpy(event, &extra, 4);
        std::memcpy(event + 4, &hash, 4);
        std::uint32_t code = 0;
        if (!GuardedAddAnimEvent(addAnimEvent, reinterpret_cast<void*>(audio), event, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            status = text;
            return false;
        }
        const std::int32_t after = AnimEventCount(audio);
        if (after != before + 1)
        {
            status = "not_queued";
            return false;
        }
        status = "queued";
        return true;
    }
    namespace
    {

        // The return RVA of a 5-byte call found by its signature.
        std::uint32_t CallReturnRva(const sig::Signature& call)
        {
            return replay::speech::ReturnRva(sig::Find(call), sig::ImageBase());
        }
    }

        const CallerSites& Callers()
        {
            static const CallerSites sites = [] {
                CallerSites s;
                s.speechCore.ai = CallReturnRva(replay::speech::kAiCoreCall);
                s.speechCore.direct = CallReturnRva(replay::speech::kDirectCoreCall);
                s.speechCore.directCtx = CallReturnRva(replay::speech::kDirectCtxCoreCall);
                s.speechController.wave = CallReturnRva(replay::speech::conversation::kWaveSoundCall);
                s.speechController.layer = CallReturnRva(replay::speech::conversation::kLayerSoundCall);
                s.footEvent.generator = CallReturnRva(footevent::kGeneratorCall);
                s.footEvent.drain = CallReturnRva(footevent::kDrainCall);
                s.footSteps.main = CallReturnRva(footevent::kMainStepCall);
                s.footSteps.secondary = CallReturnRva(footevent::kSecondaryStepCall);
                s.footSteps.wet = CallReturnRva(footevent::kWetLayerCall);
                for (std::size_t n = 0; n < replay::mountaudio::kSiteCount; ++n)
                    s.mount.rva[n] = CallReturnRva(*replay::mountaudio::kSites[n]);
                for (std::size_t n = 0; n + 1 < PlayerAudioStats::kSites; ++n)
                    s.playerAudio[n] = CallReturnRva(*kPlayerAudioSites[n]);
                s.collisionCreate = CallReturnRva(kCollisionCreateCall);
                return s;
            }();
            return sites;
        }

    const replay::speech::CorePaths& SpeechCorePaths() { return Callers().speechCore; }
    const replay::puppetaudio::SpeechControllerCallers& SpeechControllerCallers() { return Callers().speechController; }
    const footevent::StepCallers& FootStepCallers() { return Callers().footSteps; }
    namespace
    {

        // The census's call sites (sound_check_core.h CallSites). The shared
        // create-and-play queue path: the queue drain's create-and-play call,
        // the audio update's two calls of the drain, the update's caller's
        // call and the thread function's indirect call of that caller.
        const sig::Signature kDrainCreateCall{"audio_drain_create_call", soundgeneric::kQueueDrain, sig::Offset{0x11C},
            "E8 ?? ?? ?? ??", 0x258A464};
        const sig::Signature kUpdateDrainCallA{"audio_update_drain_call_a", soundworld::kAudioUpdate, sig::Offset{0xA7},
            "E8 ?? ?? ?? ??", 0x2F1CEF3};
        const sig::Signature kUpdateDrainCallB{"audio_update_drain_call_b", soundworld::kAudioUpdate,
            sig::Offset{0x3ED}, "E8 ?? ?? ?? ??", 0x2F1D239};
        const sig::Signature kUpdateCaller{"audio_update_caller",
            "48 89 5C 24 08 57 48 83 EC 20 B9 00 01 00 00 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ??", 0x2EE8648};
        const sig::Signature kUpdateCall{"audio_update_call", kUpdateCaller, sig::Offset{0x64},
            "E8 ?? ?? ?? ??", 0x2EE86AC};
        const sig::Signature kUpdateThread{"audio_update_thread",
            "48 89 5C 24 08 57 48 83 EC 70 0F 10 51 10 65 48 8B 04 25 58 00 00 00 48 8B D9", 0x25A7418};
        const sig::Signature kUpdateThreadCall{"audio_update_thread_call", kUpdateThread, sig::Offset{0xBF},
            "FF 54 24 20", 0x25A74D7};
        // The persistent cores: the creator entry (the by-ref core's call
        // target; the same bytes are a second entry's), its call of creator
        // A, and the cores' calls (the by-hash core's through the wrapper that
        // jumps to the entry).
        const sig::Signature kCreatorEntry{"audio_creator_entry", kPersistentByRef, sig::Rel32{0xEC, 0xF0},
            "4D 85 C9 74 2F 53 48 83 EC 30 49 8B D9 4D 85 C0 74 19 48 8B 44 24 60 48 89 44 24 20 E8 ?? ?? ?? ??",
            0x251686C};
        const sig::Signature kCreatorEntryCall{"audio_creator_entry_call", kCreatorEntry, sig::Offset{0x1C},
            "E8 ?? ?? ?? ??", 0x2516888};
        const sig::Signature kByRefCoreCall{"audio_by_ref_core_call", kPersistentByRef, sig::Offset{0xEB},
            "E8 ?? ?? ?? ??", 0x258920F};
        const sig::Signature kByHashCoreCall{"audio_by_hash_core_call", kPersistentByHash, sig::Offset{0x8A},
            "E8 ?? ?? ?? ??", 0x25890E2};

        // A run of the game's code: from the start of `first` to the end of
        // `last`'s function (its unwind entry). Empty when either is not
        // found, `last` is no function start or the run is out of order or
        // longer than kMaxRun (the longest is 7.6 KB: a longer one is not the
        // same layout).
        constexpr std::uint32_t kMaxRun = 0x4000;

        bool Extent(const sig::Signature& first, const sig::Signature& last, std::uint32_t& begin, std::uint32_t& end)
        {
            begin = end = 0;
            const std::uintptr_t base = sig::ImageBase();
            const std::uintptr_t start = sig::Find(first);
            const std::uintptr_t lastStart = sig::Find(last);
            if (!base || !start || !lastStart || lastStart < start)
                return false;
            DWORD64 image = 0;
            const RUNTIME_FUNCTION* entry = RtlLookupFunctionEntry(static_cast<DWORD64>(lastStart), &image, nullptr);
            if (!entry || image != base || entry->BeginAddress != lastStart - base ||
                entry->EndAddress <= entry->BeginAddress || entry->EndAddress - (start - base) > kMaxRun)
                return false;
            begin = static_cast<std::uint32_t>(start - base);
            end = entry->EndAddress;
            return true;
        }

        soundgeneric::CodeRange Run(const sig::Signature& first, const sig::Signature& last)
        {
            soundgeneric::CodeRange range;
            Extent(first, last, range.begin, range.end);
            return range;
        }
    }

    const census::CallSites& CensusCallSites()
    {
        static const census::CallSites sites = [] {
            census::CallSites s;
            const std::uintptr_t base = sig::ImageBase();
            const std::uintptr_t thread = sig::Find(kUpdateThreadCall);
            s.queue[0] = CallReturnRva(kDrainCreateCall);
            s.queue[1] = CallReturnRva(kUpdateDrainCallB);
            s.queue[2] = CallReturnRva(kUpdateDrainCallA);
            s.queue[3] = CallReturnRva(kUpdateCall);
            // call qword [rsp+20h]: four bytes.
            s.queue[4] = base && thread > base ? static_cast<std::uint32_t>(thread + 4 - base) : 0;
            s.speechWave = Callers().speechController.wave;
            s.speechLayer = Callers().speechController.layer;
            s.creatorEntry = CallReturnRva(kCreatorEntryCall);
            s.byRefCore = CallReturnRva(kByRefCoreCall);
            s.byHashCore = CallReturnRva(kByHashCoreCall);
            return s;
        }();
        return sites;
    }

    const soundgeneric::WrapperCode& GenericWrapperCode()
    {
        static const soundgeneric::WrapperCode code = [] {
            soundgeneric::WrapperCode c;
            c.creators = Run(soundgeneric::kCreatorEntries, census::kCreatorA);
            c.wrappers = Run(soundgeneric::kWrappers, soundgeneric::kQueueDrain);
            c.otherDrain = Run(soundgeneric::kOtherDrain, soundgeneric::kOtherDrain);
            c.drain = Run(soundgeneric::kQueueDrain, soundgeneric::kQueueDrain);
            return c;
        }();
        return code;
    }

    const shotdelay::FirePlay& FirePlayExtent()
    {
        static const shotdelay::FirePlay fire = [] {
            shotdelay::FirePlay f;
            Extent(kFirePlay, kFirePlay, f.begin, f.end);
            return f;
        }();
        return fire;
    }

    const soundowner::CallerRanges& OwnerCallerRanges()
    {
        static const soundowner::CallerRanges ranges = [] {
            soundowner::CallerRanges r;
            for (std::size_t n = 0; n < soundowner::kCallerRangeCount; ++n)
            {
                const soundowner::CallerFunction& f = soundowner::kCallerFunctions[n];
                r.range[n].family = f.family;
                Extent(*f.start, *f.start, r.range[n].begin, r.range[n].end);
            }
            return r;
        }();
        return ranges;
    }
}
