// audio_observer_ped.cpp: ped audio reads (holder, component, LOD, gunfight offsets, state), the speech probe, the player weapon and mount audio predicates.
// Split from audio_observer.cpp (moved code); shared internals in audio_observer_internal.h.

#include "audio_observer_internal.h"

namespace replay::audio
{
    namespace
    {

        // The ped component table both getters read (the component getter at
        // slot +0x30, the ped audio getter at +0xB0): entry = table + (index -
        // base) * 0x148.
        const sig::Signature kComponentGetter{"audio_component_getter",
            "40 53 48 83 EC 20 8B 81 9C 00 00 00 48 8B D9 4C 8B 15 ?? ?? ?? ?? 25 FF FF 01 00 2B 05 ?? ?? ?? ?? 4C 69 "
            "C8 48 01 00 00 4B 8B 44 11 30", 0xCC56B4};
        const sig::Signature kComponentTable{"audio_component_table", kComponentGetter, sig::Rel32{0x12, 0x16},
            "", 0x39ECE48};
        const sig::Signature kComponentIndexBase{"audio_component_index_base", kComponentGetter, sig::Rel32{0x1D, 0x21},
            "", 0x39ECE40};

    // Read-only sampling of the positional emitter manager (see
    // audio_observer.h EmitterSlot).
        // The site: mov rdx, rbx; lea rcx, [rip + manager]; call the emitter
        // start (its prologue checked).
        const sig::Signature kEmitterHost{"audio_emitter_host",
            "40 53 48 83 EC 20 8B 81 80 05 00 00 48 8B D9", 0x1418490};
        const sig::Signature kEmitterSite{"audio_emitter_site", kEmitterHost, sig::Offset{0x24},
            "48 8B D3 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ??", 0x14184B4};
        const sig::Signature kEmitterManager{"audio_emitter_manager", kEmitterSite, sig::Rel32{0x6, 0xA},
            "", 0x5C9A660};
        const sig::Signature kEmitterStart{"audio_emitter_start", kEmitterSite, sig::Rel32{0xB, 0xF},
            "48 89 5C 24 08 55 56 57 41 56 41 57 48 8D 6C 24 C9", 0x3212028};
        constexpr std::size_t kEmitterSlots = 128;
        constexpr std::size_t kEmitterSlotsOffset = 0x1D0;
        constexpr std::size_t kEmitterSlotSize = 0x1A0;
        constexpr std::size_t kEmitterBufferIndex = 0xD1F8;
        std::uint8_t* g_emitterManager = nullptr;
        bool g_emitterChecked = false;
        std::string g_emitterError;

        bool EnsureEmitterManager(std::string& error)
        {
            if (g_emitterChecked)
            {
                error = g_emitterError;
                return g_emitterManager != nullptr;
            }
            g_emitterChecked = true;
            const std::uintptr_t manager = sig::Find(kEmitterManager);
            if (!sig::Find(kEmitterSite) || !manager)
            {
                g_emitterError = "emitter_site_mismatch";
                error = g_emitterError;
                return false;
            }
            if (!sig::Find(kEmitterStart))
            {
                g_emitterError = "emitter_start_mismatch";
                error = g_emitterError;
                return false;
            }
            g_emitterManager = reinterpret_cast<std::uint8_t*>(manager);
            return true;
        }
    }

    bool SampleEmitterSlots(std::vector<EmitterSlot>& out, std::string& error)
    {
        out.clear();
        if (!EnsureEmitterManager(error))
            return false;
        std::uint8_t bufferIndex = 0;
        if (!GuardedRead(&bufferIndex, g_emitterManager + kEmitterBufferIndex, 1))
        {
            error = "emitter_read";
            return false;
        }
        bufferIndex &= 1;
        static std::uint8_t slot[kEmitterSlotSize];
        for (std::size_t index = 0; index < kEmitterSlots; ++index)
        {
            if (!GuardedRead(slot, g_emitterManager + kEmitterSlotsOffset + index * kEmitterSlotSize, sizeof(slot)))
                continue;
            if (!slot[0x190])
                continue;
            EmitterSlot entry;
            entry.index = static_cast<std::uint8_t>(index);
            entry.active = true;
            entry.stopping = slot[0x175] != 0;
            std::uint64_t sound = 0;
            std::memcpy(&sound, slot + 0x180, 8);
            entry.sound = sound != 0;
            std::memcpy(&entry.desc, slot + 0x160, 8);
            std::memcpy(&entry.key, slot + 0x158, 8);
            std::memcpy(&entry.entity, slot + 0x150, 8);
            std::memcpy(&entry.hash, slot + 0x168, 4);
            std::memcpy(entry.position, slot + bufferIndex * 0x10, sizeof(entry.position));
            out.push_back(entry);
        }
        return true;
    }

    bool ReadU64(std::uint64_t address, std::uint64_t& value)
    {
        value = 0;
        return address && GuardedRead(&value, reinterpret_cast<const void*>(address), 8);
    }

    bool ReadBytes(std::uint64_t address, void* out, std::size_t size)
    {
        return address && GuardedRead(out, reinterpret_cast<const void*>(address), size);
    }

    std::uint64_t SpeechEntity(void* pedEntity)
    {
        const std::uint64_t holder = PedAudioHolder(pedEntity);
        std::uint64_t speech = 0;
        if (!holder || !ReadU64(holder + 0x4438, speech))
            return 0;
        return speech;
    }

    std::uint64_t PedAudioHolder(void* pedEntity)
    {
        const std::uint64_t component = PedAudioComponent(pedEntity);
        std::uint8_t flags = 0;
        std::uint64_t holder = 0;
        if (!component || !GuardedRead(&flags, reinterpret_cast<const void*>(component + 0x218), 1) || !(flags & 2) ||
            !ReadU64(component + 0x98, holder))
        {
            return 0;
        }
        return holder;
    }

    std::uint64_t PedSoundTracker(void* pedEntity)
    {
        // The engine's tracker getter returns this component +0x10.
        const std::uint64_t component = PedAudioComponent(pedEntity);
        return component ? component + 0x10 : 0;
    }

    // The ped's row in the component table (slot +0x30 is the audio
    // component); 0 when the getter (and its two globals) is not found or a
    // read fails.
    std::uint64_t PedComponentRow(void* pedEntity)
    {
        static const std::uintptr_t indexBaseAt = sig::Find(kComponentIndexBase);
        static const std::uintptr_t tableAt = sig::Find(kComponentTable);
        if (!indexBaseAt || !tableAt || !pedEntity)
            return 0;
        std::uint32_t index = 0;
        std::uint32_t indexBase = 0;
        std::uint64_t table = 0;
        if (!GuardedRead(&index, static_cast<std::uint8_t*>(pedEntity) + 0x9C, 4) ||
            !GuardedRead(&indexBase, reinterpret_cast<const void*>(indexBaseAt), 4) ||
            !GuardedRead(&table, reinterpret_cast<const void*>(tableAt), 8) || !table)
        {
            return 0;
        }
        return table + static_cast<std::uint64_t>(static_cast<std::uint32_t>((index & 0x1FFFF) - indexBase)) * 0x148;
    }

    std::uint64_t PedAudioComponent(void* pedEntity)
    {
        const std::uint64_t row = PedComponentRow(pedEntity);
        std::uint64_t component = 0;
        if (!row || !ReadU64(row + 0x30, component) || !component)
            return 0;
        return component & ~1ull;
    }
    namespace
    {

        // Foot component (its constructor writes this vtable and clears
        // +0x450 bits 0 and 1). A ped without component slot +0x00 (every
        // NPC, so every puppet) gets footstep audio (+0x599) only through a
        // global table of two slots per ped category; a newcomer evicts the
        // occupant (clears its +0x599 and asks the foot off), which makes
        // puppets lose their footsteps mid clip. The foot audio activation
        // (mount_audio.h kCaller08) skips that table and sets +0x599 itself
        // when +0x450 bit 1 is set (kFootForceTest); no engine code sets that
        // bit, and the only other reader takes it with +0x448.
        const sig::Signature kFootCtor{"audio_foot_ctor",
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? "
            "BD 04 00 00 00 41 83 C8 FF", 0x3178EB4};
        const sig::Signature kFootVtable{"audio_foot_vtable", kFootCtor, sig::Rel32{0x1F, 0x23}, "", 0x38389B0};
        const sig::Signature kFootForceTest{"audio_foot_force_test", mountaudio::kCaller08, sig::Offset{0x8E},
            "F6 87 50 04 00 00 02", 0x319D34A};
        constexpr std::uintptr_t kFootForceByte = 0x450;
        constexpr std::uint8_t kFootForceAudio = 2;
    }

        // A foot audio's vtable is the foot component's (found once).
        bool IsFootAudio(std::uint64_t vtable)
        {
            static const std::uint64_t foot = sig::Find(kFootVtable);
            return foot && vtable == foot;
        }
    namespace
    {

        bool GuardedOrByte(std::uint8_t* target, std::uint8_t bits)
        {
            __try
            {
                *target = static_cast<std::uint8_t>(*target | bits);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool ForceFootstepAudio(void* pedEntity, std::string& status)
    {
        const std::uint64_t holder = PedAudioHolder(pedEntity);
        if (!holder)
        {
            status = "no_holder";
            return false;
        }
        const std::uint64_t foot = holder + kFootAudioOffset;
        // The byte written is the engine's own test's (kFootForceTest): no
        // write without it.
        const std::uint64_t expected = sig::Find(kFootForceTest) ? sig::Find(kFootVtable) : 0;
        std::uint64_t vtable = 0;
        if (!expected || !ReadU64(foot, vtable) || vtable != expected)
        {
            status = "foot_vtable_mismatch";
            return false;
        }
        std::uint8_t flags = 0;
        if (ReadBytes(foot + kFootForceByte, &flags, 1) && (flags & kFootForceAudio))
        {
            status = "already";
            return true;
        }
        if (!GuardedOrByte(reinterpret_cast<std::uint8_t*>(foot + kFootForceByte), kFootForceAudio))
        {
            status = "write_fault";
            return false;
        }
        status = "forced";
        return true;
    }
    namespace
    {

    // The audio LOD override of a puppet's holder. The holder's LOD
    // (mount_audio.h kCaller01: the LOD of the holder's manager, holder
    // +0x4440) starts at 4, then `cmp dword [holder+0x19A4], 4; je`
    // (kFootLodOverrideTest): any other value is the LOD, and it reports the
    // change (byte [rdx] = 1), after which the holder update asks the
    // activation of every enabled component whose max LOD the new LOD fits.
    // holder+0x19A4 is foot+0x444 (foot = holder+0x1560); the foot
    // constructor writes 4 there (no override), so a new ped in the same pool
    // block starts without it. A guarded write on the replay's own puppet
    // after the foot vtable check; any LOD, 4 writes the override off.
        const sig::Signature kFootLodOverrideTest{"audio_foot_lod_override_test", mountaudio::kCaller01,
            sig::Offset{0x53}, "44 39 B0 A4 19 00 00 74 18 C6 02 01 8B 80 A4 19 00 00", 0x317BB0F};
    }

    bool SetPuppetAudioLodValue(void* pedEntity, std::uint8_t lod, std::string& status)
    {
        const std::uint64_t holder = PedAudioHolder(pedEntity);
        if (!holder)
        {
            status = "no_holder";
            return false;
        }
        const std::uint64_t foot = holder + kFootAudioOffset;
        // The dword written is the engine's own test's (kFootLodOverrideTest):
        // no write without it.
        const std::uint64_t expected = sig::Find(kFootLodOverrideTest) ? sig::Find(kFootVtable) : 0;
        std::uint64_t vtable = 0;
        if (!expected || !ReadU64(foot, vtable) || vtable != expected)
        {
            status = "foot_vtable_mismatch";
            return false;
        }
        if (lod > 4)
        {
            status = "bad_lod";
            return false;
        }
        constexpr std::uint64_t kLodOverride = 0x444; // foot+0x444 = holder+0x19A4
        const std::uint32_t wanted = lod;
        std::uint32_t current = 0xFFFFFFFFu;
        if (!ReadBytes(foot + kLodOverride, &current, 4))
        {
            status = "read_fault";
            return false;
        }
        if (current == wanted)
        {
            status = "already";
            return true;
        }
        if (current > 4)
        {
            status = "unexpected_value"; // not a LOD: the layout is not the one proved, leave it
            return false;
        }
        if (!replay::mem::Put(static_cast<std::uintptr_t>(foot + kLodOverride), &wanted, sizeof(wanted)))
        {
            status = "write_fault";
            return false;
        }
        status = wanted == 4 ? "restored" : wanted == 0 ? "forced_lod0" : "forced_lod" + std::to_string(wanted);
        return true;
    }

    bool SetPuppetAudioLodOverride(void* pedEntity, bool on, std::string& status)
    {
        return SetPuppetAudioLodValue(pedEntity, on ? 0 : 4, status);
    }

    bool PuppetFootMaxLod(void* pedEntity, std::uint8_t& maxLod)
    {
        maxLod = 0xFF;
        const std::uint64_t holder = PedAudioHolder(pedEntity);
        return holder && ReadBytes(holder + kFootAudioOffset + 0x28, &maxLod, 1) && maxLod <= 4;
    }

    bool PedAudioLod(void* pedEntity, std::uint8_t& lod, float& metric)
    {
        lod = 0xFF;
        metric = -1.0f;
        const std::uint64_t holder = PedAudioHolder(pedEntity);
        if (!holder)
            return false;
        return ReadBytes(holder + 0x45E8, &lod, 1) && ReadBytes(holder + 0x4640, &metric, 4);
    }

    bool PedGunfightOffsets(void* pedEntity, float& volume, std::int32_t& highPass)
    {
        volume = 0.0f;
        highPass = 0;
        const std::uint64_t holder = PedAudioHolder(pedEntity);
        if (!holder)
            return false;
        return ReadBytes(holder + 0x1CF0, &volume, 4) && ReadBytes(holder + 0x1CF4, &highPass, 4);
    }

    // The audio flags dword, as the fire play reads it (bit 2 there).
        const sig::Signature kFirePlay{"audio_fire_play",
            "48 8B C4 48 89 58 10 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 98 FC FF FF 48 81 EC 30 04 00 00 0F 29 70 "
            "B8 45 33 E4", 0x2EFA7E0};
    namespace
    {
        const sig::Signature kAudioFlagsRead{"audio_flags_read", kFirePlay, sig::Offset{0x5A8},
            "8B 05 ?? ?? ?? ?? C1 E8 02 A8 01", 0x2EFAD88};
        const sig::Signature kAudioFlags{"audio_flags", kAudioFlagsRead, sig::Rel32{0x2, 0x6}, "", 0x5B9CFC0};
    }

    bool AudioGlobalFlags(std::uint32_t& flags)
    {
        flags = 0;
        const std::uintptr_t at = sig::Find(kAudioFlags);
        return at && ReadBytes(at, &flags, 4);
    }

    std::string PedAudioState(void* pedEntity)
    {
        // Evidence: holder registration and LOD, the foot component's
        // switches, and the component slots that decide them. Reads only.
        const std::uint64_t holder = PedAudioHolder(pedEntity);
        if (!holder)
            return "holder=0";
        std::uint16_t slot = 0;
        std::uint32_t mode = 0xFFFFFFFFu;
        std::uint8_t deferred = 0xFF, lod = 0xFF, forcedOff = 0xFF;
        float distanceMetric = -1.0f;
        std::uint64_t holderClass = 0;
        std::uint32_t classIndex = 0xFFFFFFFFu;
        ReadBytes(holder + 0x10, &slot, 2);
        ReadBytes(holder + 0x14, &mode, 4);
        ReadBytes(holder + 0x1AD, &deferred, 1);
        ReadBytes(holder + 0x45E8, &lod, 1);
        ReadBytes(holder + 0x45FC, &forcedOff, 1);
        ReadBytes(holder + 0x4640, &distanceMetric, 4);
        if (ReadU64(holder + 0x4608, holderClass) && holderClass)
            ReadBytes(holderClass + 0x0C, &classIndex, 4);

        const std::uint64_t foot = holder + kFootAudioOffset;
        std::uint32_t footFlags = 0xFFFFFFFFu, footOverride = 0xFFFFFFFFu, footForce = 0xFFFFFFFFu, footForceBits = 0xFFFFFFFFu;
        std::uint8_t footActive = 0xFF, footMaxLod = 0xFF, footGround = 0xFF;
        ReadBytes(foot + 0x20, &footFlags, 4);
        ReadBytes(foot + 0x24, &footActive, 1);
        ReadBytes(foot + 0x28, &footMaxLod, 1);
        ReadBytes(foot + 0x444, &footOverride, 4);
        ReadBytes(foot + 0x448, &footForce, 4);
        ReadBytes(foot + 0x450, &footForceBits, 4);
        ReadBytes(foot + 0x599, &footGround, 1);

        const std::uint64_t row = PedComponentRow(pedEntity);
        std::uint64_t slot00 = 0, slot88 = 0, slotB8 = 0;
        std::uint32_t slotB8Flags = 0xFFFFFFFFu;
        std::uint16_t pedC4 = 0xFFFF;
        if (row)
        {
            ReadU64(row + 0x00, slot00);
            ReadU64(row + 0x88, slot88);
            ReadU64(row + 0xB8, slotB8);
            if (slotB8)
                ReadBytes((slotB8 & ~1ull) + 0x40, &slotB8Flags, 4);
        }
        ReadBytes(reinterpret_cast<std::uint64_t>(pedEntity) + 0xC4, &pedC4, 2);

        char text[512];
        std::snprintf(text, sizeof(text),
            "slot=0x%X,mode=%u,deferred=%u,lod=%u,forced_off=%u,dist=%.2f,class=%d,"
            "foot_flags=0x%X,foot_active=%u,foot_max_lod=%u,foot_override=%u,foot_force=%u,foot_force_bits=0x%X,foot_ground=%u,"
            "c00=%d,c88=%d,cB8=%d,cB8_40=0x%X,ped_c4=%u",
            slot, mode, deferred, lod, forcedOff, distanceMetric, static_cast<int>(classIndex),
            footFlags, footActive, footMaxLod, footOverride, footForce, footForceBits, footGround,
            slot00 ? 1 : 0, slot88 ? 1 : 0, slotB8 ? 1 : 0, slotB8Flags, pedC4);
        return text;
    }

    bool ProbeSpeech(void* pedEntity, SpeechProbe& out, std::string& error)
    {
        std::uint8_t* speech = reinterpret_cast<std::uint8_t*>(SpeechEntity(pedEntity));
        if (!speech)
        {
            error = "no_speech_entity";
            return false;
        }
        out.speechEntity = reinterpret_cast<std::uint64_t>(speech);
        GuardedRead(&out.lastContext, speech + 0x388, 4);
        std::uint8_t* list = nullptr;
        GuardedRead(&list, speech + 0x78, sizeof(list));
        GuardedRead(&out.lines, speech + 0x80, sizeof(out.lines));
        if (!list || out.lines == 0 || out.lines > 64)
            return true;
        std::uint8_t* line = nullptr;
        if (!GuardedRead(&line, list, sizeof(line)) || !line)
            return true;
        out.line.resize(0x200);
        if (!GuardedRead(out.line.data(), line, out.line.size()))
            out.line.clear();
        std::uint8_t* data = nullptr;
        if (GuardedRead(&data, line + 0x1D8, sizeof(data)) && data)
        {
            out.lineData.resize(0x140);
            if (!GuardedRead(out.lineData.data(), data, out.lineData.size()))
                out.lineData.clear();
        }
        return true;
    }

    void CollectMelee(std::uint64_t fromUs, std::uint64_t toUs, std::vector<MeleeCall>& out)
    {
        Collect(g_melee, kMeleeRing, g_nextMelee, fromUs, toUs, out);
    }
    namespace
    {
        // The twelve callers' calls (their return addresses, call + 5, are the
        // counters' sites): the fire play (x2), the fire queue consumer, the
        // player-or-its-vehicle test (weapon settings, fire sound choice, fire
        // event, projectile audio; it has twins, so it is reached from the
        // projectile audio's call), the settings variant, the auto fire, four
        // other weapon callers (the fourth twice).
        const sig::Signature kPlayerAudioSite01{"audio_player_audio_site_01", kFirePlay, sig::Offset{0x110},
            "E8 ?? ?? ?? ??", 0x2EFA8F0};
        // The predicate, from this call: its first bytes are where another
        // module's detour would be, and its tail is the component read that
        // other predicates inline.
        const sig::Signature kPlayerAudioPredicate{"audio_player_weapon_predicate", kPlayerAudioSite01,
            sig::Rel32{0x01, 0x05},
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? FF 01 00 2B 05 ?? ?? ?? ?? 48 69 C8 48 01 00 00 48 8B 05 ?? ?? "
            "?? ?? 48 8B 94 01 B8 00 00 00 48 8B CA 48 83 E1 FE 48 F7 DA 48 1B C0 48 23 C1 8A 48 40 B0 01 84 C8 75 05 "
            "F6 C1 02 75 02 32 C0 C3",
            0x2F02F8C};
        const sig::Signature kPlayerAudioSite02{"audio_player_audio_site_02", kFirePlay, sig::Offset{0x13E},
            "E8 ?? ?? ?? ??", 0x2EFA91E};
        const sig::Signature kFireQueue{"audio_fire_queue",
            "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC 98 00 00 00 0F 29 70 A8 48 63 05 ?? ?? "
            "?? ??", 0x2F0DB9C};
        const sig::Signature kPlayerAudioSite03{"audio_player_audio_site_03", kFireQueue, sig::Offset{0x2B9},
            "E8 ?? ?? ?? ??", 0x2F0DE55};
        const sig::Signature kProjectileAudio{"audio_projectile_audio",
            "48 89 5C 24 10 55 56 57 41 56 41 57 48 8D 6C 24 C9 48 81 EC E0 00 00 00", 0x118391C};
        const sig::Signature kPlayerOrVehicle{"audio_player_or_vehicle", kProjectileAudio, sig::Rel32{0x73, 0x77},
            "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 E8 ?? ?? ?? ?? 32 DB 84 C0 75 0C 48 8B CF E8 ?? ?? ?? ?? 3A C3 74 "
            "02 B3 01 8A C3 48 8B 5C 24 30 48 83 C4 20 5F C3", 0x2F030C0};
        const sig::Signature kPlayerAudioSite04{"audio_player_audio_site_04", kPlayerOrVehicle, sig::Offset{0xD},
            "E8 ?? ?? ?? ??", 0x2F030CD};
        const sig::Signature kSettingsVariant{"audio_settings_variant",
            "40 53 48 83 EC 20 33 DB 48 85 C9 74 0F E8 ?? ?? ?? ??", 0x3110424};
        const sig::Signature kPlayerAudioSite05{"audio_player_audio_site_05", kSettingsVariant, sig::Offset{0xD},
            "E8 ?? ?? ?? ??", 0x3110431};
        const sig::Signature kAutoFire{"audio_auto_fire",
            "48 8B C4 48 89 58 20 4C 89 40 18 48 89 50 10 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 58 FD FF FF",
            0x2F17D84};
        const sig::Signature kPlayerAudioSite06{"audio_player_audio_site_06", kAutoFire, sig::Offset{0x1A3},
            "E8 ?? ?? ?? ??", 0x2F17F27};
        const sig::Signature kWeaponCallerA{"audio_weapon_caller_a",
            "48 89 5C 24 08 57 48 83 EC 20 80 3D ?? ?? ?? ?? 00 48 8B FA 48 8B D9 74 69", 0x2EFB1F0};
        const sig::Signature kPlayerAudioSite07{"audio_player_audio_site_07", kWeaponCallerA, sig::Offset{0x2A},
            "E8 ?? ?? ?? ??", 0x2EFB21A};
        const sig::Signature kWeaponCallerB{"audio_weapon_caller_b",
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 54 41 56 41 57 48 83 EC 70 33 DB 0F 29 70 D8",
            0x2EEDA7C};
        const sig::Signature kPlayerAudioSite08{"audio_player_audio_site_08", kWeaponCallerB, sig::Offset{0x8F},
            "E8 ?? ?? ?? ??", 0x2EEDB0B};
        const sig::Signature kWeaponCallerC{"audio_weapon_caller_c",
            "48 8B C4 48 89 58 08 48 89 70 20 55 57 41 54 41 56 41 57 48 8D A8 78 FE FF FF 48 81 EC 60 02 00 00 48 8B "
            "72 10", 0x2F09768};
        const sig::Signature kPlayerAudioSite09{"audio_player_audio_site_09", kWeaponCallerC, sig::Offset{0x1E1},
            "E8 ?? ?? ?? ??", 0x2F09949};
        const sig::Signature kWeaponCallerD{"audio_weapon_caller_d",
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 54 41 55 41 56 41 57 48 83 EC 50 44 8A 25 ?? ?? ?? ??",
            0x2F0D428};
        const sig::Signature kPlayerAudioSite10{"audio_player_audio_site_10", kWeaponCallerD, sig::Offset{0x9B},
            "E8 ?? ?? ?? ??", 0x2F0D4C3};
        const sig::Signature kPlayerAudioSite11{"audio_player_audio_site_11", kWeaponCallerD, sig::Offset{0x324},
            "E8 ?? ?? ?? ??", 0x2F0D74C};
        const sig::Signature kWeaponCallerE{"audio_weapon_caller_e",
            "48 8B C4 48 89 58 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A9 48 81 EC D0 00 00 00", 0x2F19FD0};
        const sig::Signature kPlayerAudioSite12{"audio_player_audio_site_12", kWeaponCallerE, sig::Offset{0xF9},
            "E8 ?? ?? ?? ??", 0x2F1A0C9};
    }
        const sig::Signature* const kPlayerAudioSites[PlayerAudioStats::kSites - 1] = {&kPlayerAudioSite01,
            &kPlayerAudioSite02, &kPlayerAudioSite03, &kPlayerAudioSite04, &kPlayerAudioSite05, &kPlayerAudioSite06,
            &kPlayerAudioSite07, &kPlayerAudioSite08, &kPlayerAudioSite09, &kPlayerAudioSite10, &kPlayerAudioSite11,
            &kPlayerAudioSite12};
    namespace
    {
        constexpr std::size_t kPlayerAudioPeds = 4;
        std::atomic<std::uint64_t> g_playerAudioPeds[kPlayerAudioPeds]{};
    }
        std::atomic<PlayerAudioPredicateFn> g_originalPlayerAudio{nullptr};
        void* g_playerAudioTarget = nullptr;
    namespace
    {
        std::atomic<std::uint64_t> g_playerAudioCalls{0};
        std::atomic<std::uint64_t> g_playerAudioEngineTrue{0};
        std::atomic<std::uint64_t> g_playerAudioOverrides{0};
        std::atomic<std::uint64_t> g_playerAudioSiteTrue[PlayerAudioStats::kSites]{};

        void CountPlayerAudioSite(std::uintptr_t returnAddress)
        {
            const std::uintptr_t rva = returnAddress - g_base;
            const CallerSites& callers = Callers();
            std::size_t site = PlayerAudioStats::kSites - 1;
            for (std::size_t index = 0; index + 1 < PlayerAudioStats::kSites; ++index)
            {
                if (callers.playerAudio[index] && callers.playerAudio[index] == rva)
                {
                    site = index;
                    break;
                }
            }
            g_playerAudioSiteTrue[site].fetch_add(1, std::memory_order_relaxed);
        }

        bool __fastcall PlayerAudioPredicateDetour(const void* entity)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            g_playerAudioCalls.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t address = reinterpret_cast<std::uint64_t>(entity);
            if (address)
            {
                for (const std::atomic<std::uint64_t>& ped : g_playerAudioPeds)
                {
                    if (ped.load(std::memory_order_acquire) == address)
                    {
                        g_playerAudioOverrides.fetch_add(1, std::memory_order_relaxed);
                        CountPlayerAudioSite(reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
                        return true;
                    }
                }
            }
            const PlayerAudioPredicateFn original = g_originalPlayerAudio.load(std::memory_order_acquire);
            const bool result = original ? original(entity) : false;
            if (result)
            {
                g_playerAudioEngineTrue.fetch_add(1, std::memory_order_relaxed);
                CountPlayerAudioSite(reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
            }
            return result;
        }
    }

    bool InstallPlayerWeaponAudio(std::string& error)
    {
        if (g_playerAudioTarget)
            return true;
        if (!g_base)
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        Callers(); // the counters' sites, before the detour runs
        return Hook(kPlayerAudioPredicate, reinterpret_cast<void*>(&PlayerAudioPredicateDetour), g_originalPlayerAudio,
            g_playerAudioTarget, "player_weapon_audio", error);
    }

    void SetPlayerAudioPeds(const std::vector<std::uint64_t>& peds)
    {
        for (std::size_t index = 0; index < kPlayerAudioPeds; ++index)
            g_playerAudioPeds[index].store(index < peds.size() ? peds[index] : 0, std::memory_order_release);
    }

    int PedAudioPlayerFlag(void* pedEntity)
    {
        std::uint8_t type = 0;
        if (!pedEntity || !GuardedRead(&type, static_cast<std::uint8_t*>(pedEntity) + 0x30, 1))
            return -1;
        if (type != 4)
            return 0;
        const std::uint64_t row = PedComponentRow(pedEntity);
        std::uint64_t component = 0;
        if (!row || !ReadU64(row + kPlayerAudioComponentSlot, component))
            return -1;
        component &= ~1ull;
        std::uint8_t flags = 0;
        if (!component || !GuardedRead(&flags, reinterpret_cast<const void*>(component + kPlayerAudioFlags), 1))
            return -1;
        return (flags & 1) == 0 && (flags & 2) != 0 ? 1 : 0;
    }

    bool PlayerAudioPublished(std::uint64_t ped)
    {
        if (!ped)
            return false;
        for (const std::atomic<std::uint64_t>& published : g_playerAudioPeds)
            if (published.load(std::memory_order_acquire) == ped)
                return true;
        return false;
    }

    PlayerAudioStats PlayerAudioSnapshot()
    {
        PlayerAudioStats stats;
        stats.calls = g_playerAudioCalls.load(std::memory_order_relaxed);
        stats.engineTrue = g_playerAudioEngineTrue.load(std::memory_order_relaxed);
        stats.overrides = g_playerAudioOverrides.load(std::memory_order_relaxed);
        stats.installed = g_playerAudioTarget != nullptr;
        for (std::size_t index = 0; index < PlayerAudioStats::kSites; ++index)
            stats.siteTrue[index] = g_playerAudioSiteTrue[index].load(std::memory_order_relaxed);
        return stats;
    }

    std::string PlayerAudioSites(const PlayerAudioStats& now, const PlayerAudioStats& since)
    {
        std::string text;
        for (std::size_t index = 0; index < PlayerAudioStats::kSites; ++index)
        {
            const std::uint64_t count = now.siteTrue[index] - since.siteTrue[index];
            if (!count)
                continue;
            char item[40]{};
            if (index + 1 < PlayerAudioStats::kSites)
                std::snprintf(item, sizeof(item), "%s0x%X:%llu", text.empty() ? "" : "|",
                    static_cast<unsigned>(Callers().playerAudio[index]), static_cast<unsigned long long>(count));
            else
                std::snprintf(item, sizeof(item), "%sother:%llu", text.empty() ? "" : "|",
                    static_cast<unsigned long long>(count));
            text += item;
        }
        return text.empty() ? "none" : text;
    }
    namespace
    {

    // The player's horse variant of the ped audio (mount_audio.h).
    // The predicate, whole function (0x54 bytes, ends in ret):
    //   ent = [rcx+0B0h] ; the entity linked to the slot +0xA8 component
    //   no when ent = 0
    //   row = [table] + ((dword [ent+9Ch] & 1FFFFh) - [base index]) * 148h
    //   comp = [row+0B8h] & ~1 ; flags = byte [comp+40h]
    //   yes when !(flags & 1) && (flags & 2)
        static_assert(sizeof(MountAudioStats::siteTrue) / sizeof(std::uint64_t) == replay::mountaudio::kCounterSlots,
            "one counter per site and other");
        // Reached from the mount audio's first call of it (mount_audio.h): its
        // first bytes are where another module's detour would be, and its tail
        // is the component read that other predicates inline.
        const sig::Signature kMountAudioPredicate{"audio_player_mount_predicate", replay::mountaudio::kSite01,
            sig::Rel32{0x01, 0x05},
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 9C 00 00 00 25 FF FF 01 00 2B 05 ?? ?? ?? ?? 48 69 C8 48 01 00 "
            "00 48 8B 05 ?? ?? ?? ?? 48 8B 94 01 B8 00 00 00 48 8B CA 48 83 E1 FE 48 F7 DA 48 1B C0 48 23 C1 8A 48 40 "
            "B0 01 84 C8 75 05 F6 C1 02 75 02 32 C0 C3",
            0x8B69B0};
        constexpr std::size_t kMountLinkEntity = 0xB0;
    }
        std::atomic<std::uint64_t> g_mountAudioComponents[kMountAudioComponents]{};
        std::atomic<MountAudioPredicateFn> g_originalMountAudio{nullptr};
        void* g_mountAudioTarget = nullptr;
    namespace
    {
        std::atomic<std::uint64_t> g_mountAudioCalls{0};
        std::atomic<std::uint64_t> g_mountAudioEngineTrue{0};
        std::atomic<std::uint64_t> g_mountAudioOverrides{0};
        std::atomic<std::uint64_t> g_mountAudioSiteTrue[replay::mountaudio::kCounterSlots]{};

        bool __fastcall MountAudioPredicateDetour(const void* component)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            g_mountAudioCalls.fetch_add(1, std::memory_order_relaxed);
            const std::uintptr_t returnRva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_base;
            const std::size_t slot = replay::mountaudio::SiteSlot(Callers().mount, static_cast<std::uint32_t>(returnRva));
            const std::uint64_t address = reinterpret_cast<std::uint64_t>(component);
            if (address && slot < replay::mountaudio::kSiteCount)
            {
                for (const std::atomic<std::uint64_t>& published : g_mountAudioComponents)
                {
                    if (published.load(std::memory_order_acquire) == address)
                    {
                        g_mountAudioOverrides.fetch_add(1, std::memory_order_relaxed);
                        g_mountAudioSiteTrue[slot].fetch_add(1, std::memory_order_relaxed);
                        return true;
                    }
                }
            }
            const MountAudioPredicateFn original = g_originalMountAudio.load(std::memory_order_acquire);
            const bool result = original ? original(component) : false;
            if (result)
            {
                g_mountAudioEngineTrue.fetch_add(1, std::memory_order_relaxed);
                g_mountAudioSiteTrue[slot].fetch_add(1, std::memory_order_relaxed);
            }
            return result;
        }
    }

    bool InstallPlayerMountAudio(std::string& error)
    {
        if (g_mountAudioTarget)
            return true;
        if (!g_base)
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        Callers(); // the thirteen sites, before the detour runs
        return Hook(kMountAudioPredicate, reinterpret_cast<void*>(&MountAudioPredicateDetour), g_originalMountAudio,
            g_mountAudioTarget, "player_mount_audio", error);
    }

    void SetPlayerMountAudioComponents(const std::vector<std::uint64_t>& components)
    {
        for (std::size_t index = 0; index < kMountAudioComponents; ++index)
            g_mountAudioComponents[index].store(index < components.size() ? components[index] : 0,
                std::memory_order_release);
    }

    bool MountAudioPublished(std::uint64_t component)
    {
        if (!component)
            return false;
        for (const std::atomic<std::uint64_t>& published : g_mountAudioComponents)
            if (published.load(std::memory_order_acquire) == component)
                return true;
        return false;
    }

    std::uint64_t MountLinkComponent(void* pedEntity)
    {
        const std::uint64_t row = PedComponentRow(pedEntity);
        std::uint64_t component = 0;
        if (!row || !ReadU64(row + kMountLinkSlot, component))
            return 0;
        return component & ~1ull;
    }

    int MountLinkIsPlayer(void* pedEntity, std::uint64_t& linked)
    {
        linked = 0;
        const std::uint64_t component = MountLinkComponent(pedEntity);
        if (!component || !ReadU64(component + kMountLinkEntity, linked))
            return -1;
        if (!linked)
            return 0;
        return PedAudioPlayerFlag(reinterpret_cast<void*>(linked));
    }

    MountAudioStats MountAudioSnapshot()
    {
        MountAudioStats stats;
        stats.calls = g_mountAudioCalls.load(std::memory_order_relaxed);
        stats.engineTrue = g_mountAudioEngineTrue.load(std::memory_order_relaxed);
        stats.overrides = g_mountAudioOverrides.load(std::memory_order_relaxed);
        stats.installed = g_mountAudioTarget != nullptr;
        for (std::size_t index = 0; index < replay::mountaudio::kCounterSlots; ++index)
            stats.siteTrue[index] = g_mountAudioSiteTrue[index].load(std::memory_order_relaxed);
        return stats;
    }

    std::string MountAudioSites(const MountAudioStats& now, const MountAudioStats& since)
    {
        std::string text;
        for (std::size_t index = 0; index < replay::mountaudio::kCounterSlots; ++index)
        {
            const std::uint64_t count = now.siteTrue[index] - since.siteTrue[index];
            if (!count)
                continue;
            char item[40]{};
            if (index < replay::mountaudio::kSiteCount)
                std::snprintf(item, sizeof(item), "%s0x%X:%llu", text.empty() ? "" : "|",
                    static_cast<unsigned>(Callers().mount.rva[index]), static_cast<unsigned long long>(count));
            else
                std::snprintf(item, sizeof(item), "%sother:%llu", text.empty() ? "" : "|",
                    static_cast<unsigned long long>(count));
            text += item;
        }
        return text.empty() ? "none" : text;
    }
}
