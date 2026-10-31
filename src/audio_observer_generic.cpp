// audio_observer_generic.cpp: the generic sound player: ports, settings, create and queue, bank probe, play, stop and the sound state.
// Split from audio_observer.cpp (moved code); shared internals in audio_observer_internal.h.

#include "audio_observer_internal.h"

namespace replay::audio
{
    namespace
    {

    // ---- The generic sound player's ports ----

        // The ports the generic player calls that the rest of the mod does
        // not, each found by its signature (S13 grown past its masked
        // displacements to be found once, and its tail jump's target found
        // too). The read probe's hook table has the same bytes
        // (soundfx_probe.cpp kHooks).
        enum GenericPort : int
        {
            kGenericS10 = 0, // prepare
            kGenericS13,     // stop
            kGenericS18,     // volume
            kGenericS19,     // pitch
            kGenericS21,     // low-pass cutoff
            kGenericS22,     // high-pass cutoff
            kGenericS23,     // post submix attenuation
            kGenericS24,     // volume curve scale
            kGenericPortCount
        };
        struct GenericPortSpec
        {
            const char* id;
            const sig::Signature* port;
            const sig::Signature* check; // null: none
        };
        const sig::Signature kGenericPortS10{"audio_generic_s10",
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 30 33 DB 41 8B E9 45 8A F0 48 8B "
            "FA 48 8B F1",
            0x257CB78};
        const sig::Signature kGenericPortS13{"audio_generic_s13",
            "0F B7 91 D6 00 00 00 81 FA FF FF 00 00 74 2A 0F B6 81 A0 00 00 00 44 8B 05 ?? ?? ?? ?? 48 69 C8 60 A8 02 "
            "00 44 0F AF C2 48 8B 05 ?? ?? ?? ?? 4C 03 84 01 50 A8 02 00 EB 03 45 33 C0 B2 01", 0x2585BE4};
        const sig::Signature kGenericPortS13Tail{"audio_generic_s13_tail", kGenericPortS13, sig::Offset{0x41},
            "E9 ?? ?? ?? ??", 0x2585C25};
        const sig::Signature kGenericPortS13TailTarget{"audio_generic_s13_tail_target", kGenericPortS13Tail,
            sig::Rel32{0x1, 0x5}, "44 8B 05 ?? ?? ?? ?? B8 FF BF 00 00 49 C1 E0 06", 0x2536BEC};
        const sig::Signature kGenericPortS18{"audio_generic_s18",
            "8B 05 ?? ?? ?? ?? 48 C1 E0 06 F3 0F 11 8C 08 20 01 00 00 C3", 0x2537168};
        const sig::Signature kGenericPortS19{"audio_generic_s19",
            "8B 05 ?? ?? ?? ?? 48 C1 E0 06 66 89 94 08 3C 01 00 00 C3", 0x2536E34};
        const sig::Signature kGenericPortS21{"audio_generic_s21",
            "41 B8 64 00 00 00 41 3B D0 72 0D 41 B8 20 4E 00 00 41 3B D0 44 0F 46 C2 8B 05 ?? ?? ?? ?? 48 C1 E0 06 66 "
            "44 89 84 08 44 01 00 00 C3", 0x2536C70};
        const sig::Signature kGenericPortS22{"audio_generic_s22",
            "8B 05 ?? ?? ?? ?? 41 B8 5C 5D 00 00 41 3B D0 44 0F 46 C2 48 C1 E0 06 66 44 89 84 08 46 01 00 00 C3",
            0x2536A0C};
        const sig::Signature kGenericPortS23{"audio_generic_s23",
            "8B 05 ?? ?? ?? ?? 48 C1 E0 06 F3 0F 11 8C 08 24 01 00 00 C3", 0x2536E5C};
        const sig::Signature kGenericPortS24{"audio_generic_s24",
            "8B 05 ?? ?? ?? ?? 48 C1 E0 06 F3 0F 11 8C 08 34 01 00 00 C3", 0x253717C};
        const GenericPortSpec kGenericPorts[kGenericPortCount] = {
            {"S10", &kGenericPortS10, nullptr},
            {"S13", &kGenericPortS13, &kGenericPortS13TailTarget},
            {"S18", &kGenericPortS18, nullptr},
            {"S19", &kGenericPortS19, nullptr},
            {"S21", &kGenericPortS21, nullptr},
            {"S22", &kGenericPortS22, nullptr},
            {"S23", &kGenericPortS23, nullptr},
            {"S24", &kGenericPortS24, nullptr},
        };
        void* g_genericPort[kGenericPortCount] = {};
        bool g_genericPortResolved = false;
        std::string g_genericPortStatus;


        void ResolveGenericPorts()
        {
            if (g_genericPortResolved)
                return;
            g_genericPortResolved = true;
            std::string status;
            for (int n = 0; n < kGenericPortCount; ++n)
            {
                const GenericPortSpec& spec = kGenericPorts[n];
                const std::uintptr_t at = sig::Find(*spec.port);
                // A port the read probe (or the generic recorder) hooked:
                // its bytes start with the hook's jump, the trampoline is the function.
                void* probed = at ? replay::soundfx::OriginalFor(at - sig::ImageBase()) : nullptr;
                const char* result = "ok";
                if (probed)
                    g_genericPort[n] = probed;
                else if (!at)
                    result = "signature_mismatch";
                else if (spec.check && !sig::Find(*spec.check))
                    result = "second_check_mismatch";
                else
                    g_genericPort[n] = reinterpret_cast<void*>(at);
                status += std::string(n ? ";" : "") + spec.id + ":" + (probed ? "trampoline" : result);
            }
            g_genericPortStatus = status;
        }

        void* GenericPortFor(GenericPort port)
        {
            ResolveGenericPorts();
            return g_genericPort[port];
        }

        bool CallSettingF32(const engine::Function<std::uint64_t(std::uint64_t, float)>& f, GenericPort port,
            std::uint64_t sound, float value)
        {
            const auto set = reinterpret_cast<std::uintptr_t>(GenericPortFor(port));
            const std::uint64_t settings = set ? PortSettings(sound, kLinkAdjust) : 0;
            if (!set || !settings)
                return false;
            std::uint64_t result = 0;
            return engine::Call(f, set, result, settings, value);
        }

        bool CallSettingInt(const engine::Function<std::uint64_t(std::uint64_t, std::uint64_t)>& f, GenericPort port,
            std::uint64_t sound, std::uint64_t value)
        {
            const auto set = reinterpret_cast<std::uintptr_t>(GenericPortFor(port));
            const std::uint64_t settings = set ? PortSettings(sound, kLinkAdjust) : 0;
            if (!set || !settings)
                return false;
            std::uint64_t result = 0;
            return engine::Call(f, set, result, settings, value);
        }

        // The packet's fields over the constructor's defaults
        // (sound_generic_core.h offsets: the same the census reads).
        void WriteGenericInit(std::uint8_t* params, const GenericSoundInit& init)
        {
            if (init.hasPosition)
                std::memcpy(params + 0x00, init.position, sizeof(init.position));
            std::memcpy(params + 0xA4, &init.volume, 4);
            std::memcpy(params + 0xA8, &init.volume2, 4);
            std::memcpy(params + 0xAC, &init.pitch, 2);
            std::memcpy(params + 0xAE, &init.pan, 2);
            params[0x13A] = init.flags;
            const std::int32_t count = init.variableCount > 3 ? 3 : init.variableCount;
            for (std::int32_t n = 0; n < count; ++n)
            {
                std::memcpy(params + 0x10C + n * 8, &init.variableHash[n], 4);
                std::memcpy(params + 0x10C + n * 8 + 4, &init.variableValue[n], 4);
            }
            std::memcpy(params + 0x124, &count, 4);
            // The init variables (+0x10, 16 pairs, count +0x90, the layout the
            // constructor zeroes and S39 appends to). The
            // variable block sounds of the tree take them at Init: the skid's
            // speed (0x48fe569d) and its two switches.
            const std::int32_t vars = init.initVariableCount > 16 ? 16 : init.initVariableCount;
            for (std::int32_t n = 0; n < vars; ++n)
            {
                std::memcpy(params + 0x10 + n * 8, &init.initVariableHash[n], 4);
                std::memcpy(params + 0x10 + n * 8 + 4, &init.initVariableValue[n], 4);
            }
            std::memcpy(params + 0x90, &vars, 4);
        }

        // The value dwords first (the fields above write the same values
        // again); only value offsets, never the engine's pointers.
        void WriteGenericValues(std::uint8_t* params, const GenericSoundInit& init)
        {
            const std::uint8_t count = init.valueCount > 35 ? 35 : init.valueCount;
            for (std::uint8_t n = 0; n < count; ++n)
            {
                const std::size_t offset = static_cast<std::size_t>(init.valueIndex[n]) * 4;
                if (offset < 0x94 || offset + 4 > kSoundParamsBytes || (offset >= 0xC8 && offset < 0xE8))
                    continue;
                if (offset == 0x12C) // the u16 handle stays the constructor's
                    std::memcpy(params + 0x12E, reinterpret_cast<const std::uint8_t*>(&init.valueRaw[n]) + 2, 2);
                else
                    std::memcpy(params + offset, &init.valueRaw[n], 4);
            }
        }

        // The environment group of an entity, the call the ped request queue
        // makes for a request's owner (create flag 0): it asks the entity's
        // audio (its vtable +0xF0) for the group (that object's vtable +0x78).
        // Found once, SEH.
        const sig::Signature kEntityEnvGroup{"audio_entity_env_group",
            "40 53 48 83 EC 20 48 8B 01 8A DA FF 90 F0 00 00 00 48 8B C8 33 C0 48 85 C9 74 09 4C 8B 01 8A D3 41 FF 50 "
            "78 48 83 C4 20 5B C3", 0xE383E8};
        std::uint64_t EntityEnvironmentGroup(std::uint64_t entity, const char*& status)
        {
            static int checked = 0; // 0 not yet, 1 matched, -1 mismatch
            static std::uintptr_t fn = 0;
            if (!checked)
            {
                fn = sig::Find(kEntityEnvGroup);
                checked = fn ? 1 : -1;
            }
            if (checked < 0 || !fn)
            {
                status = "env_signature_mismatch";
                return 0;
            }
            std::uint64_t vtable = 0;
            if (!entity || (entity & 7u) != 0 || !GuardedRead(&vtable, reinterpret_cast<const void*>(entity), 8) ||
                vtable < g_base || vtable >= g_imageEnd)
            {
                status = "env_entity_not_an_object";
                return 0;
            }
            std::uint64_t group = 0;
            if (!engine::Call(engine::kEntityEnvGroup, fn, group, entity, 0))
            {
                status = "env_exception";
                return 0;
            }
            if (group && (group & 7u) != 0)
            {
                status = "env_misaligned";
                return 0;
            }
            status = group ? "env_given" : "env_none";
            return group;
        }
    }

    std::uint64_t EnvironmentGroupOf(void* entity, const char*& status)
    {
        if (!g_base)
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!g_imageEnd)
        {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
            g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
        }
        return EntityEnvironmentGroup(reinterpret_cast<std::uint64_t>(entity), status);
    }

    bool ReadGenericSettings(std::uint64_t sound, GenericSettingsView& out)
    {
        const std::uint64_t settings = PortSettings(sound, kLinkRead);
        if (!settings)
            return false;
        std::uint8_t flags = 0;
        if (!GuardedRead(&out.clientVariable, reinterpret_cast<const void*>(settings + 0x22C), 4) ||
            !GuardedRead(&out.envGroup, reinterpret_cast<const void*>(settings + 0x248), 8) ||
            !GuardedRead(&flags, reinterpret_cast<const void*>(settings + 0x260), 1))
            return false;
        out.updatesWithEntity = (flags & 0x10u) != 0;
        return true;
    }

    bool CreateGenericSound(std::uint64_t audio, std::uint64_t soundRef, bool byHash, GenericSoundInit& init,
        bool play, std::uint64_t* slot, std::string& error)
    {
        static std::uintptr_t playFn = 0;
        static std::uintptr_t release = 0;
        if (!playFn)
        {
            void* port = PrepareAndPlayPort();
            const std::uintptr_t free = sig::Find(kSoundParamsRelease);
            if (!port || !free)
            {
                error = "persistent_signature_mismatch";
                return false;
            }
            playFn = reinterpret_cast<std::uintptr_t>(port);
            release = free;
        }
        // Through the trampolines: the replayed create is not recorded again.
        const auto byRef = reinterpret_cast<std::uintptr_t>(g_originalByRef.load(std::memory_order_acquire));
        const auto byHashFn = reinterpret_cast<std::uintptr_t>(g_originalByHash.load(std::memory_order_acquire));
        const auto construct = reinterpret_cast<std::uintptr_t>(g_originalCtor.load(std::memory_order_acquire));
        if (!construct || (byHash ? !byHashFn : !byRef) || !audio || !slot)
        {
            error = "observer_not_installed";
            return false;
        }
        {
            // An audEntity is polymorphic: its first field is a vtable in
            // the image whose first slot is code in the image.
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
        // The owner's environment group, as the game's request queue gives one.
        init.envGroupGiven = 0;
        init.envGroupStatus = "not_asked";
        if (init.envGroupEntity)
            init.envGroupGiven = EntityEnvironmentGroup(init.envGroupEntity, init.envGroupStatus);
        struct Context
        {
            std::uintptr_t byRef; std::uintptr_t byHash; bool useHash; std::uintptr_t construct; std::uintptr_t release;
            std::uintptr_t play; bool doPlay; std::uint64_t audio; std::uint64_t soundRef; const GenericSoundInit* init;
            std::uint64_t* slot;
        };
        Context context{byRef, byHashFn, byHash, construct, release, playFn, play, audio, soundRef, &init, slot};
        std::uint32_t code = 0;
        const bool ok = GuardedCall([](void* raw) {
            auto* c = static_cast<Context*>(raw);
            alignas(16) std::uint8_t params[kSoundParamsBytes + 0x40] = {};
            // A faulted call raises again: GuardedCall reports its code.
            void* constructed = nullptr;
            if (!engine::Call(engine::kSoundParamsCtor, c->construct, constructed, params))
                engine::Raise();
            WriteGenericValues(params, *c->init); // the game's value fields
            WriteGenericInit(params, *c->init); // tracker [0xC8, 0xE8) left clear: the player drives the position
            // The group goes in +0xE0; the creator hands it to S30, which
            // counts the use (the settings' release gives it back).
            std::memcpy(params + 0xE0, &c->init->envGroupGiven, 8);
            const bool called = c->useHash
                ? engine::Call(engine::kPersistentByHash, c->byHash, c->audio, static_cast<std::uint32_t>(c->soundRef),
                      reinterpret_cast<std::uint64_t>(c->slot), reinterpret_cast<std::uint64_t>(params))
                : engine::Call(engine::kPersistentByRef, c->byRef, c->audio, c->soundRef, reinterpret_cast<std::uint64_t>(c->slot),
                      reinterpret_cast<std::uint64_t>(params));
            if (!called || !engine::Call(engine::kSoundParamsRelease, c->release, params + 0x12C))
                engine::Raise();
            if (*c->slot && c->doPlay && !engine::Call(engine::kPrepareAndPlay, c->play, *c->slot, 0, 0, 0, 0))
                engine::Raise();
        }, &context, code);
        if (!ok)
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        error = *slot ? (play ? "playing" : "created") : "not_created";
        if (!*slot)
        {
            // Why, as PlayDirectSound tells it (the public entry, the core and
            // the persistent cores): the audio entity's state
            // (+0x14, 0 refuses) and registration (+0x10, 0xFFFF refuses);
            // "audio_ok" leaves the core's other checks (the sound ref, the
            // sound pool).
            std::uint32_t state = 0xFFFFFFFFu;
            std::uint16_t registered = 0;
            if (GuardedRead(&state, reinterpret_cast<const void*>(audio + 0x14), 4) &&
                GuardedRead(&registered, reinterpret_cast<const void*>(audio + 0x10), 2))
                error += state == 0 ? ":audio_state0" : registered == 0xFFFF ? ":audio_unregistered" : ":audio_ok";
        }
        return *slot != 0;
    }

    // The creator's sound for a waiting ticket. Sounds made on a silenced
    // puppet's audio that no ticket of ours took (the puppet's own, let
    // through by the port).
    std::atomic<std::uint64_t> g_unticketedPuppet{0};
    std::atomic<std::uint64_t> g_unticketedPuppetRef[8] = {};
    std::atomic<std::uint32_t> g_unticketedPuppetOwner[8] = {};
    std::uint64_t UnticketedPuppetCreates(std::string& samples)
    {
        samples.clear();
        const std::uint64_t count = g_unticketedPuppet.load(std::memory_order_relaxed);
        char part[48]{};
        for (std::uint64_t n = 0; n < count && n < 8; ++n)
        {
            const std::uint32_t owner = g_unticketedPuppetOwner[n].load(std::memory_order_relaxed);
            std::snprintf(part, sizeof(part), "%s%u:%u@0x%llx", samples.empty() ? "" : "|", owner >> 16, owner & 0xFFFFu,
                static_cast<unsigned long long>(g_unticketedPuppetRef[n].load(std::memory_order_relaxed)));
            samples += part;
        }
        return count;
    }
    void ResetUnticketedPuppetCreates() { g_unticketedPuppet.store(0, std::memory_order_relaxed); }

    void NoteQueueBirth(std::uint64_t audio, std::uint64_t soundRef, std::uint64_t sound, std::uint64_t out)
    {
        const auto unticketed = [&]() {
            std::uint32_t owner = replay::puppetaudio::kNoOwner;
            if (!sound || !audio || !SilencedPuppetAudioOwner(audio, owner))
                return;
            const std::uint64_t n = g_unticketedPuppet.fetch_add(1, std::memory_order_relaxed);
            if (n < 8)
            {
                g_unticketedPuppetRef[n].store(soundRef, std::memory_order_relaxed);
                g_unticketedPuppetOwner[n].store(owner, std::memory_order_relaxed);
            }
        };
        // A create with a persistent reference is never ours (mode c's sounds
        // are born without one): it takes no ticket.
        if (out)
            return;
        if (!sound || !audio || g_ticketsWaiting.load(std::memory_order_relaxed) == 0)
        {
            unticketed();
            return;
        }
        const std::uint64_t now = QpcUs();
        std::size_t best = kQueueTickets;
        std::uint64_t bestId = 0;
        for (std::size_t n = 0; n < kQueueTickets; ++n)
        {
            if (g_ticketAudio[n].load(std::memory_order_acquire) != audio ||
                g_ticketRef[n].load(std::memory_order_relaxed) != soundRef ||
                g_ticketUntilUs[n].load(std::memory_order_relaxed) < now)
                continue;
            const std::uint64_t id = g_ticketId[n].load(std::memory_order_relaxed);
            if (best == kQueueTickets || id < bestId)
            {
                best = n;
                bestId = id;
            }
        }
        if (best == kQueueTickets)
        {
            unticketed();
            return;
        }
        std::uint64_t expected = audio;
        if (!g_ticketAudio[best].compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
            return; // another creator took it
        replay::ring::Writer<QueueBirth> slot(g_queueBirths, kQueueBirthRing, g_queueBirthNext);
        slot.value.ticket = bestId;
        slot.value.sound = sound;
        slot.value.out = out;
        slot.value.bornUs = now;
        slot.Publish();
    }

    void CancelQueueTicket(std::uint64_t ticket)
    {
        if (!ticket)
            return;
        const std::size_t n = static_cast<std::size_t>(ticket % kQueueTickets);
        if (g_ticketId[n].load(std::memory_order_relaxed) == ticket)
            g_ticketAudio[n].store(0, std::memory_order_release);
    }

    std::uint64_t QueueBirthCursor() { return g_queueBirthNext.load(std::memory_order_acquire); }

    void CollectQueueBirthsSince(std::uint64_t& cursor, std::vector<QueueBirth>& out)
    {
        CollectSince(g_queueBirths, kQueueBirthRing, g_queueBirthNext, cursor, out);
    }

    bool QueueGenericSound(std::uint64_t audio, std::uint64_t soundRef, const std::uint8_t* recorded,
        GenericSoundInit& init, std::string& error, std::uint64_t* ticket)
    {
        static std::uintptr_t play = 0;
        static std::uintptr_t release = 0;
        if (!play)
        {
            const std::uintptr_t create = sig::Find(kCreateAndPlayPublic), free = sig::Find(kSoundParamsRelease);
            if (!create || !free)
            {
                error = "queue_signature_mismatch";
                return false;
            }
            play = create;
            release = free;
        }
        const auto construct = reinterpret_cast<std::uintptr_t>(g_originalCtor.load(std::memory_order_acquire));
        if (!construct || !audio)
        {
            error = "observer_not_installed";
            return false;
        }
        {
            // An audEntity object (as CreateGenericSound checks it).
            std::uint64_t vtable = 0;
            if ((audio & 7u) != 0 || !GuardedRead(&vtable, reinterpret_cast<const void*>(audio), sizeof(vtable)) ||
                vtable < g_base || vtable >= g_imageEnd)
            {
                error = "audio_entity_not_an_object";
                return false;
            }
        }
        // The owner's environment group, as CreateGenericSound.
        init.envGroupGiven = 0;
        init.envGroupStatus = "not_asked";
        if (init.envGroupEntity)
            init.envGroupGiven = EntityEnvironmentGroup(init.envGroupEntity, init.envGroupStatus);
        ExpectQueuedCreate(audio, soundRef);
        const std::uint64_t opened = OpenQueueTicket(audio, soundRef);
        if (ticket)
            *ticket = opened;
        struct Context
        {
            std::uintptr_t play; std::uintptr_t construct; std::uintptr_t release; std::uint64_t audio; std::uint64_t soundRef;
            const std::uint8_t* recorded; const GenericSoundInit* init; bool queued;
        };
        Context context{play, construct, release, audio, soundRef, recorded, &init, false};
        std::uint32_t code = 0;
        bool ok = false;
        {
            ReplayEmitScope emit; // the S03 refusal of a silenced puppet's own one-shot lets the replay's through
            ok = GuardedCall([](void* raw) {
                auto* c = static_cast<Context*>(raw);
                alignas(16) std::uint8_t params[kSoundParamsBytes + 0x40] = {};
                // A faulted call raises again: GuardedCall reports its code.
                void* constructed = nullptr;
                if (!engine::Call(engine::kSoundParamsCtor, c->construct, constructed, params))
                    engine::Raise();
                if (c->recorded)
                    CopyRecordedSoundParams(params, c->recorded);
                else
                {
                    WriteGenericValues(params, *c->init);
                    WriteGenericInit(params, *c->init);
                }
                const std::uint64_t none = 0;
                std::memcpy(params + 0xC8, &none, 8); // no tracker
                if (c->init->envGroupGiven) // the game's weapon rattle does the same
                    std::memcpy(params + 0xE0, &c->init->envGroupGiven, 8);
                if (!engine::Call(engine::kCreateAndPlayPublic, c->play, c->queued, c->audio, c->soundRef,
                        reinterpret_cast<std::uint64_t>(params)) ||
                    !engine::Call(engine::kSoundParamsRelease, c->release, params + 0x12C))
                    engine::Raise();
            }, &context, code);
        }
        if (!ok)
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            CancelQueueTicket(opened);
            if (ticket)
                *ticket = 0;
            return false;
        }
        error = context.queued ? "queued" : "not_queued";
        if (!context.queued)
        {
            CancelQueueTicket(opened);
            if (ticket)
                *ticket = 0;
            std::uint32_t state = 0xFFFFFFFFu;
            std::uint16_t registered = 0;
            if (GuardedRead(&state, reinterpret_cast<const void*>(audio + 0x14), 4) &&
                GuardedRead(&registered, reinterpret_cast<const void*>(audio + 0x10), 2))
                error += state == 0 ? ":audio_state0" : registered == 0xFFFF ? ":audio_unregistered" : ":audio_ok";
        }
        return context.queued;
    }

    bool PrepareSound(std::uint64_t sound, std::string& error)
    {
        const auto prepare = reinterpret_cast<std::uintptr_t>(GenericPortFor(kGenericS10));
        if (!prepare)
        {
            error = "port_unverified";
            return false;
        }
        if (!sound || !RequestedSettings(sound))
        {
            error = "no_sound";
            return false;
        }
        if (!PortSettings(sound, kLinkPrepare)) // freed or reused
        {
            error = "sound_gone";
            return false;
        }
        // (sound, no wave slot, may load, priority 0), as the game's own calls.
        std::uint64_t result = 0;
        std::uint32_t code = 0;
        if (!engine::Call(engine::kGenericS10, prepare, result, sound, 0, 1, 0))
        {
            engine::Failed(code);
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        error = "prepared";
        return true;
    }
    namespace
    {

        // The bank probe's functions.
        const sig::Signature kRequiredDynamicBank{"audio_required_dynamic_bank",
            "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 48 8B EC 48 83 EC 40 49 8B F0 48 8D 05 ?? ?? "
            "?? ?? 48 8B F9 48 89 45 E0 45 33 F6 4C 8D 45 E0 48 8D 0D ?? ?? ?? ?? 4C 89 75 E8 44 89 75 F0 E8 ?? ?? ?? "
            "??",
            0x2EF341C};
        const sig::Signature kFindBankSlot{"audio_find_bank_slot",
            "48 89 5C 24 08 57 48 83 EC 20 8B D9 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ??", 0x2545B1C};
        const sig::Signature kBankLoadStatus{"audio_bank_load_status",
            "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 8B DA 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B D3 48 8B CF E8 ?? ?? "
            "?? ?? 48 8D 0D ?? ?? ?? ?? 8B D8", 0x2549530};
        // The banks a ref needs do not change: one metadata walk per ref.
        std::unordered_map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> g_bankOfRef;
    }

    bool ProbeSoundBank(std::uint64_t soundRef, BankProbe& out, std::string& error)
    {
        out = BankProbe{};
        static int checked = 0; // 0 not yet, 1 ok, -1 refused
        static std::uintptr_t required = 0;
        static std::uintptr_t findSlot = 0;
        static std::uintptr_t loadStatus = 0;
        if (checked == 0)
        {
            required = sig::Find(kRequiredDynamicBank);
            findSlot = sig::Find(kFindBankSlot);
            loadStatus = sig::Find(kBankLoadStatus);
            checked = required && findSlot && loadStatus ? 1 : -1;
        }
        if (checked < 0)
        {
            error = "bank_probe_signature_mismatch";
            return false;
        }
        if (!soundRef)
        {
            error = "no_ref";
            return false;
        }
        struct Context
        {
            std::uintptr_t required; std::uintptr_t findSlot; std::uintptr_t loadStatus;
            std::uint64_t ref; bool known; std::uint32_t bank; std::uint32_t count; std::uint64_t slot; std::uint32_t status;
        };
        Context context{required, findSlot, loadStatus, soundRef, false, 0xFFFFFFFFu, 0, 0, 0xFFFFFFFFu};
        if (const auto found = g_bankOfRef.find(soundRef); found != g_bankOfRef.end())
        {
            context.known = true;
            context.bank = found->second.first;
            context.count = found->second.second;
        }
        std::uint32_t code = 0;
        if (!GuardedCall([](void* raw) {
                auto* c = static_cast<Context*>(raw);
                // A faulted call raises again: GuardedCall reports its code.
                std::uint32_t* bank = nullptr;
                if (!c->known && !engine::Call(engine::kRequiredDynamicBank, c->required, bank, &c->bank, c->ref, &c->count))
                    engine::Raise();
                if (c->count == 0)
                    return;
                if (!engine::Call(engine::kFindBankSlot, c->findSlot, c->slot, c->bank))
                    engine::Raise();
                if (c->slot && !engine::Call(engine::kBankLoadStatus, c->loadStatus, c->status, c->slot, c->bank))
                    engine::Raise();
            }, &context, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        if (!context.known)
        {
            if (g_bankOfRef.size() > 4096)
                g_bankOfRef.clear();
            g_bankOfRef[soundRef] = {context.count ? context.bank : 0xFFFFFFFFu, context.count};
        }
        out.bank = context.count ? context.bank : 0xFFFFFFFFu;
        out.count = context.count;
        out.slotFound = context.slot != 0;
        out.status = context.status;
        error = "probed";
        return true;
    }

    const char* BankStateName(const BankProbe& probe)
    {
        if (probe.count == 0)
            return "static";
        if (!probe.slotFound)
            return "absent";
        switch (probe.status)
        {
        case 0: return "loaded";
        case 1: return "loading";
        case 2: return "failed";
        case 3: return "not_requested";
        default: return "unknown";
        }
    }

    bool PlaySound(std::uint64_t sound, std::string& error)
    {
        static std::uintptr_t play = 0;
        if (!play)
        {
            play = reinterpret_cast<std::uintptr_t>(PrepareAndPlayPort());
            if (!play)
            {
                error = "persistent_signature_mismatch";
                return false;
            }
        }
        if (!sound || !RequestedSettings(sound))
        {
            error = "no_sound";
            return false;
        }
        if (!PortSettings(sound, kLinkPlay)) // freed or reused
        {
            error = "sound_gone";
            return false;
        }
        std::uint32_t code = 0;
        if (!engine::Call(engine::kPrepareAndPlay, play, sound, 0, 0, 0, 0))
        {
            engine::Failed(code);
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        error = "playing";
        return true;
    }

    bool StopSound(std::uint64_t sound, std::string& error)
    {
        const auto stop = reinterpret_cast<std::uintptr_t>(GenericPortFor(kGenericS13));
        if (!stop)
        {
            error = "port_unverified";
            return false;
        }
        if (!sound || !RequestedSettings(sound))
        {
            error = "no_sound";
            return false;
        }
        if (!PortSettings(sound, kLinkStop)) // freed or reused
        {
            error = "sound_gone";
            return false;
        }
        std::uint64_t result = 0;
        std::uint32_t code = 0;
        if (!engine::Call(engine::kGenericS13, stop, result, sound))
        {
            engine::Failed(code);
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        error = "stopped";
        return true;
    }

    // The identity of a live sound and its check.
    std::uint64_t SoundIdentity(std::uint64_t sound) { return sound ? LiveSettings(sound) : 0; }
    bool SoundIdentityHolds(std::uint64_t sound, std::uint64_t identity)
    {
        return sound && identity && LiveSettings(sound) == identity;
    }
    bool SoundPlayState(std::uint64_t sound, std::uint64_t identity, int& state)
    {
        state = -1;
        std::uint8_t flags = 0;
        if (!SoundIdentityHolds(sound, identity) || !GuardedRead(&flags, reinterpret_cast<const void*>(sound + 0xF2), 1))
            return false;
        state = flags >> 6;
        return true;
    }
    SoundLinkStats SoundLinkCounters()
    {
        SoundLinkStats out;
        out.checks = g_linkChecks.load(std::memory_order_relaxed);
        out.notSound = g_linkNotSound.load(std::memory_order_relaxed);
        out.noSettings = g_linkNoSettings.load(std::memory_order_relaxed);
        out.broken = g_linkBroken.load(std::memory_order_relaxed);
        for (int n = 0; n < kSoundLinkPorts; ++n)
            out.refused[n] = g_linkRefused[n].load(std::memory_order_relaxed);
        out.persistentBroken = g_persistentLinkBroken.load(std::memory_order_relaxed);
        return out;
    }

    bool SetSoundVolume(std::uint64_t sound, float volume) { return CallSettingF32(engine::kGenericS18, kGenericS18, sound, volume); }
    bool SetSoundPitch(std::uint64_t sound, std::int16_t pitch)
    {
        return CallSettingInt(engine::kGenericS19, kGenericS19, sound, static_cast<std::uint64_t>(static_cast<std::uint16_t>(pitch)));
    }
    bool SetSoundLowPass(std::uint64_t sound, std::uint32_t cutoff) { return CallSettingInt(engine::kGenericS21, kGenericS21, sound, cutoff); }
    bool SetSoundHighPass(std::uint64_t sound, std::uint32_t cutoff) { return CallSettingInt(engine::kGenericS22, kGenericS22, sound, cutoff); }
    bool SetSoundPostSubmix(std::uint64_t sound, float attenuation) { return CallSettingF32(engine::kGenericS23, kGenericS23, sound, attenuation); }
    bool SetSoundVolumeCurveScale(std::uint64_t sound, float scale) { return CallSettingF32(engine::kGenericS24, kGenericS24, sound, scale); }

    std::string GenericSoundPortStatus()
    {
        ResolveGenericPorts();
        return g_genericPortStatus;
    }
}
