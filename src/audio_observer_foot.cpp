// audio_observer_foot.cpp: footsteps: the foot detours, the foot material, wetness, mix and layers replay, footsteps and foot events played, the movement volume.
// Split from audio_observer.cpp (moved code); shared internals in audio_observer_internal.h.

#include "audio_observer_internal.h"

namespace replay::audio
{
    namespace
    {
        // Its reads of the stack arguments (engine_calls.h): a build that
        // changed them fails here.
        const sig::Signature kFootstepArg7{"audio_footstep_arg7", kFootstep, sig::Offset{0x116}, "8B 45 60", 0x317A456};
        const sig::Signature kFootstepArg6{"audio_footstep_arg6", kFootstep, sig::Offset{0x11C}, "0A 4D 58", 0x317A45C};
        const sig::Signature* const kFootstepArgs[] = {&kFootstepArg7, &kFootstepArg6};
        constexpr std::uint32_t kFootstepRing = 1024;
        Slot<FootstepCall> g_footsteps[kFootstepRing];
        std::atomic<std::uint64_t> g_nextFootstep{0};

        bool IsSilencedFoot(const void* footAudio)
        {
            const auto value = reinterpret_cast<std::uint64_t>(footAudio);
            const std::uint32_t count = g_silencedFeetCount.load(std::memory_order_acquire);
            for (std::uint32_t n = 0; n < count && n < kMaxSilencedFeet; ++n)
                if (g_silencedFeet[n].load(std::memory_order_relaxed) == value)
                    return true;
            return false;
        }

        // foot_material.h FootMix: guarded read and write of a foot's mix
        // slot (+0x390 volume, +0x3B0 high-pass, +0x3C0 low-pass).
        bool ReadFootMix(const std::uint8_t* footAudio, std::uint8_t foot, footmaterial::FootMix& out)
        {
            out = footmaterial::FootMix{};
            if (!footAudio || foot >= footmaterial::kFeet)
                return false;
            float volume = 0.0f;
            std::uint32_t highPass = 0;
            std::uint32_t lowPass = 0;
            if (!GuardedRead(&volume, footAudio + footmaterial::MixVolumeOffset(foot), sizeof(volume)) ||
                !GuardedRead(&highPass, footAudio + footmaterial::MixHighPassOffset(foot), sizeof(highPass)) ||
                !GuardedRead(&lowPass, footAudio + footmaterial::MixLowPassOffset(foot), sizeof(lowPass)) ||
                !footmaterial::PlausibleMix(volume, highPass, lowPass))
                return false;
            out.known = true;
            out.volume = volume;
            out.highPass = highPass;
            out.lowPass = lowPass;
            return true;
        }

        // The foot fields the replay writes (foot_material.h), each by the
        // engine instruction that reads it: the slot ((foot + 9) << 6 = +0x240
        // + 0x40 * foot) and its container +0x0, name hash +0x8 and ref +0xC
        // (the slot writer's stores), the ground layers +0x3D0 and the wetness
        // +0x380 (the main step), the mix +0x390, +0x3B0, +0x3C0 and the foot
        // rise +0xF0 + 0x90 * foot + 0x70 of the row +0x88 component (the step
        // sound). A field whose instruction is not found is not written.
        const sig::Signature kFootSlotRead{"audio_foot_slot_read", footevent::kMainStep, sig::Offset{0x24},
            "49 83 C1 09 0F 29 70 C8 49 C1 E1 06", 0x318D1DC};
        const sig::Signature kFootLayerRead{"audio_foot_layer_read", footevent::kMainStep, sig::Offset{0xDF},
            "F3 0F 10 84 8F D0 03 00 00", 0x318D297};
        const sig::Signature kFootWetnessRead{"audio_foot_wetness_read", footevent::kMainStep, sig::Offset{0x180},
            "F3 0F 10 BC 87 80 03 00 00", 0x318D338};
        const sig::Signature kFootSlotWriter{"audio_foot_slot_writer",
            "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 20", 0x3188614};
        const sig::Signature kFootSlotFields{"audio_foot_slot_fields", kFootSlotWriter, sig::Offset{0x12D},
            "41 89 5F 08 E8 ?? ?? ?? ?? 48 8B C8 48 8D 55 40 41 83 C9 FF 44 8B C3 E8 ?? ?? ?? ?? 8B 08 89 4D 40 41 89 "
            "4F 0C C7 45 44 FF FF 0F 00 48 8B 4D 40 E8 ?? ?? ?? ?? 48 85 C0 74 05 49 89 07", 0x3188741};
        const sig::Signature kFootMixVolumeRead{"audio_foot_mix_volume_read", kAudioStepSound, sig::Offset{0x1C0},
            "F3 42 0F 58 8C 87 90 03 00 00", 0x318D6EC};
        const sig::Signature kFootMixFilterRead{"audio_foot_mix_filter_read", kAudioStepSound, sig::Offset{0x24A},
            "8B 8C 97 B0 03 00 00 3B C1 0F 46 C1 89 45 58 8B 8C 97 C0 03 00 00", 0x318D776};
        const sig::Signature kFootImpactBase{"audio_foot_impact_base", kAudioStepSound, sig::Offset{0x8C},
            "49 8D 9C 24 F0 00 00 00 48 8D 0C C0 48 C1 E1 04 48 03 D9", 0x318D5B8};
        const sig::Signature kFootImpactRead{"audio_foot_impact_read", kAudioStepSound, sig::Offset{0x3E9},
            "F3 0F 10 43 70", 0x318D915};
    }

        // Resolved once (first by Install, before any detour runs).
        const FootFields& FootProof()
        {
            static const FootFields fields = [] {
                FootFields f;
                f.slot = sig::Find(kFootSlotRead) && sig::Find(kFootSlotFields);
                f.layers = sig::Find(kFootLayerRead) != 0;
                f.wetness = sig::Find(kFootWetnessRead) != 0;
                f.mix = sig::Find(kFootMixVolumeRead) && sig::Find(kFootMixFilterRead);
                f.impact = sig::Find(kFootImpactBase) && sig::Find(kFootImpactRead);
                return f;
            }();
            return fields;
        }
    namespace
    {

        bool WriteFootMix(std::uint8_t* footAudio, std::uint8_t foot, const footmaterial::FootMix& mix)
        {
            if (!footAudio || foot >= footmaterial::kFeet || !mix.known || !FootProof().mix)
                return false;
            return replay::mem::Put(reinterpret_cast<std::uintptr_t>(footAudio + footmaterial::MixVolumeOffset(foot)),
                       &mix.volume, sizeof(mix.volume)) &&
                replay::mem::Put(reinterpret_cast<std::uintptr_t>(footAudio + footmaterial::MixHighPassOffset(foot)),
                    &mix.highPass, sizeof(mix.highPass)) &&
                replay::mem::Put(reinterpret_cast<std::uintptr_t>(footAudio + footmaterial::MixLowPassOffset(foot)),
                    &mix.lowPass, sizeof(mix.lowPass));
        }

        // foot_material.h FootDrive: the row +0x88 component of the foot
        // audio's ped ([foot +0x18]).
        std::uint64_t LocomotionComponent(const std::uint8_t* footAudio)
        {
            std::uint64_t ped = 0;
            if (!footAudio || !GuardedRead(&ped, footAudio + 0x18, 8) || !ped)
                return 0;
            const std::uint64_t row = PedComponentRow(reinterpret_cast<void*>(ped));
            std::uint64_t component = 0;
            if (!row || !GuardedRead(&component, reinterpret_cast<const void*>(row + 0x88), 8))
                return 0;
            return component & ~1ull;
        }

        bool ReadFootBase(const std::uint8_t* footAudio, float& out)
        {
            return footAudio && GuardedRead(&out, footAudio + footmaterial::kBaseVolumeOffset, sizeof(out)) &&
                footmaterial::PlausibleBase(out);
        }

        void ReadFootDrive(const std::uint8_t* footAudio, std::uint8_t foot, footmaterial::FootDrive& out)
        {
            out = footmaterial::FootDrive{};
            out.baseKnown = ReadFootBase(footAudio, out.base);
            const std::uint64_t component = foot < footmaterial::kFeet ? LocomotionComponent(footAudio) : 0;
            out.impactKnown = component &&
                GuardedRead(&out.impact, reinterpret_cast<const void*>(component + footmaterial::ImpactOffset(foot)), sizeof(out.impact)) &&
                footmaterial::PlausibleImpact(out.impact);
        }

        bool WriteFootImpact(const std::uint8_t* footAudio, std::uint8_t foot, float impact)
        {
            const std::uint64_t component =
                foot < footmaterial::kFeet && FootProof().impact ? LocomotionComponent(footAudio) : 0;
            float current = 0.0f;
            if (!component || !GuardedRead(&current, reinterpret_cast<const void*>(component + footmaterial::ImpactOffset(foot)), sizeof(current)))
                return false;
            if (current == impact)
                return false;
            return replay::mem::Put(component + footmaterial::ImpactOffset(foot), &impact, sizeof(impact));
        }
    }

        void __fastcall FootstepDetour(void* footAudio, std::int32_t eventId, std::uint32_t arg3,
            std::uint8_t foot, std::uint8_t arg5, std::uint8_t flag, std::uint32_t arg7, std::uint8_t arg8)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.audio_footstep");
            InFlight inFlight;
            if (t_animScopeDepth)
                ++t_animFootsteps;
            // The foot-down handler only queues when the foot audio is enabled
            // (+0x24, its own test): a step it drops made no sound in the game and
            // is not recorded.
            std::uint8_t enabled = 0;
            if (!footAudio || !GuardedRead(&enabled, static_cast<std::uint8_t*>(footAudio) + 0x24, 1) || !enabled)
            {
                const FootstepFn original = g_originalFootstep.load(std::memory_order_acquire);
                ReplayedSystemScope scope;
                if (original)
                    original(footAudio, eventId, arg3, foot, arg5, flag, arg7, arg8);
                return;
            }
            FootstepCall call;
            call.timeUs = QpcUs();
            call.footAudio = reinterpret_cast<std::uint64_t>(footAudio);
            call.eventId = eventId;
            call.arg3 = arg3;
            call.foot = foot;
            call.arg5 = arg5;
            call.flag = flag;
            call.arg7 = arg7;
            call.arg8 = arg8;
            // foot_material.h FootMix: the slot of this foot as the step will
            // read it (the quadruped's hit pattern wrote it right
            // before calling this entry).
            ReadFootMix(static_cast<const std::uint8_t*>(footAudio), foot, call.mix);
            ReadFootDrive(static_cast<const std::uint8_t*>(footAudio), foot, call.drive);
            // foot_material.h PuppetAudioLodAt: the holder's audio LOD
            // (+0x45E8, holder = foot - 0x1560) now.
            {
                std::uint8_t lod = footmaterial::kLodUnknown;
                if (GuardedRead(&lod, static_cast<const std::uint8_t*>(footAudio) - kFootAudioOffset + 0x45E8, 1) &&
                    lod <= footmaterial::kLodNoOverride)
                    call.audioLod = lod;
            }
            // A puppet's own foot-down (its animation, not the recording) is
            // kept in the ring as evidence but not played: the replay plays the
            // recorded steps, otherwise a gallop gets extra hooves out of step.
            call.silenced = IsSilencedFoot(footAudio);
            replay::ring::Writer<FootstepCall> slot(g_footsteps, kFootstepRing, g_nextFootstep);
            slot.value = call;
            slot.Publish();
            if (call.silenced)
            {
                g_silencedFootsteps.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const FootstepFn original = g_originalFootstep.load(std::memory_order_acquire);
            ReplayedSystemScope scope;
            if (original)
                original(footAudio, eventId, arg3, foot, arg5, flag, arg7, arg8);
        }
    namespace
    {

        // foot_material.h: the standing material of the feet. Record: the
        // slot of each queued step's foot at the movement update entry (the
        // container the step is about to play with). Playback: the recorded
        // material of each puppet foot, set again on its slot at the movement
        // update entry (its own ground probe may have refreshed it since
        // PlayFootstep). The table is written by the script thread only and
        // read here with a version (odd = writing).
        constexpr std::uint32_t kFootMaterialRing = 1024;
        Slot<footmaterial::Sample> g_footMaterialSamples[kFootMaterialRing];
        std::atomic<std::uint64_t> g_nextFootMaterialSample{0};
        constexpr std::size_t kMaxFootMaterialFeet = 256;
        struct FootMaterialEntry
        {
            std::atomic<std::uint64_t> footAudio{0};
            std::atomic<std::uint32_t> version{0};
            std::uint64_t container[footmaterial::kFeet] = {};
            footmaterial::Material material[footmaterial::kFeet] = {};
            footmaterial::FootMix mix[footmaterial::kFeet] = {};
            footmaterial::FootDrive drive[footmaterial::kFeet] = {};
        };
        FootMaterialEntry g_footMaterials[kMaxFootMaterialFeet];
        std::atomic<std::uint32_t> g_footMaterialCount{0};
        std::atomic<std::uint64_t> g_footMaterialDetourWrites{0};
        std::atomic<std::uint64_t> g_footMaterialSampled{0};
        std::atomic<std::uint64_t> g_footWetnessDetourWrites{0};
        std::atomic<bool> g_footWetnessReplay{true};             // ini FootWetnessReplay
        std::atomic<std::uint64_t> g_footMixDetourWrites{0};
        std::atomic<bool> g_footImpactReplay{true};              // ini FootImpactReplay
        std::atomic<std::uint64_t> g_footImpactDetourWrites{0};
        std::atomic<bool> g_footLayerReplay{true};               // ini FootLayerReplay
        std::atomic<std::uint64_t> g_footLayerDetourWrites{0};

        struct FootSlot
        {
            std::uint64_t container = 0;
            footmaterial::Material material;
        };

        bool ReadFootSlot(const std::uint8_t* footAudio, std::uint8_t foot, FootSlot& out)
        {
            std::uint8_t raw[16] = {};
            if (foot >= footmaterial::kFeet || !GuardedRead(raw, footAudio + footmaterial::SlotOffset(foot), sizeof(raw)))
                return false;
            std::memcpy(&out.container, raw + footmaterial::kSlotContainer, 8);
            std::memcpy(&out.material.hash, raw + footmaterial::kSlotHash, 4);
            std::memcpy(&out.material.ref, raw + footmaterial::kSlotRef, 4);
            return true;
        }

        bool WriteFootSlot(std::uint8_t* footAudio, std::uint8_t foot, std::uint64_t container,
            const footmaterial::Material& material)
        {
            std::uint8_t raw[16] = {};
            std::memcpy(raw + footmaterial::kSlotContainer, &container, 8);
            std::memcpy(raw + footmaterial::kSlotHash, &material.hash, 4);
            std::memcpy(raw + footmaterial::kSlotRef, &material.ref, 4);
            return foot < footmaterial::kFeet && FootProof().slot &&
                replay::mem::Put(reinterpret_cast<std::uintptr_t>(footAudio + footmaterial::SlotOffset(foot)), raw, sizeof(raw));
        }

        // The queued steps (count +0xB0, entries +0x70, foot at +0xC), at
        // most kQueueMax; false when the queue is empty or unreadable.
        std::uint32_t QueuedFeet(const std::uint8_t* footAudio, std::uint8_t (&feet)[footmaterial::kQueueMax])
        {
            std::int32_t count = 0;
            if (!GuardedRead(&count, footAudio + footmaterial::kQueueCount, sizeof(count)) || count <= 0)
                return 0;
            if (count > footmaterial::kQueueMax)
                count = footmaterial::kQueueMax;
            std::uint32_t got = 0;
            for (std::int32_t n = 0; n < count; ++n)
            {
                std::uint8_t foot = 0xFF;
                if (GuardedRead(&foot, footAudio + footmaterial::kQueue + footmaterial::kQueueStride * n + footmaterial::kQueueFoot, 1))
                    feet[got++] = foot;
            }
            return got;
        }

        void SampleFootMaterials(const std::uint8_t* footAudio, const std::uint8_t* feet, std::uint32_t count)
        {
            const std::uint64_t now = QpcUs();
            for (std::uint32_t n = 0; n < count; ++n)
            {
                FootSlot current;
                if (!ReadFootSlot(footAudio, feet[n], current))
                    continue;
                footmaterial::Sample sample;
                sample.timeUs = now;
                sample.footAudio = reinterpret_cast<std::uint64_t>(footAudio);
                sample.foot = feet[n];
                sample.material = current.material;
                // foot_material.h kWetnessBase: the wetness this step reads
                // (kFootWetnessRead), left by the previous foot update.
                float wetness = -1.0f;
                if (feet[n] < footmaterial::kFeet &&
                    GuardedRead(&wetness, footAudio + footmaterial::WetnessOffset(feet[n]), sizeof(wetness)) &&
                    (!(wetness >= 0.0f) || wetness > 16.0f))
                    wetness = -1.0f;
                sample.material.wetness = wetness;
                // foot_material.h kLayerBase: the ground layer weights this
                // step reads (kFootLayerRead), written by the foot update's
                // [vtable +0x80] just before this entry.
                if (feet[n] < footmaterial::kFeet)
                {
                    float layers[footmaterial::kLayers] = {};
                    bool known = GuardedRead(layers, footAudio + footmaterial::LayerOffset(feet[n], 0), sizeof(layers));
                    for (std::uint8_t l = 0; l < footmaterial::kLayers && known; ++l)
                        known = footmaterial::PlausibleLayer(layers[l]);
                    sample.material.layersKnown = known;
                    if (known)
                        std::memcpy(sample.material.layers, layers, sizeof(layers));
                }
                replay::ring::Writer<footmaterial::Sample> slot(g_footMaterialSamples, kFootMaterialRing, g_nextFootMaterialSample);
                slot.value = sample;
                slot.Publish();
                g_footMaterialSampled.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void ApplyFootMaterials(std::uint8_t* footAudio)
        {
            const auto key = reinterpret_cast<std::uint64_t>(footAudio);
            const std::uint32_t count = g_footMaterialCount.load(std::memory_order_acquire);
            for (std::uint32_t n = 0; n < count && n < kMaxFootMaterialFeet; ++n)
            {
                FootMaterialEntry& entry = g_footMaterials[n];
                if (entry.footAudio.load(std::memory_order_acquire) != key)
                    continue;
                const std::uint32_t before = entry.version.load(std::memory_order_acquire);
                if (before & 1)
                    return;
                std::uint64_t container[footmaterial::kFeet];
                footmaterial::Material material[footmaterial::kFeet];
                footmaterial::FootMix mix[footmaterial::kFeet];
                footmaterial::FootDrive drive[footmaterial::kFeet];
                std::memcpy(drive, entry.drive, sizeof(drive));
                std::memcpy(container, entry.container, sizeof(container));
                std::memcpy(material, entry.material, sizeof(material));
                std::memcpy(mix, entry.mix, sizeof(mix));
                std::atomic_thread_fence(std::memory_order_acquire);
                if (entry.version.load(std::memory_order_relaxed) != before)
                    return;
                for (std::uint8_t foot = 0; foot < footmaterial::kFeet; ++foot)
                {
                    FootSlot current;
                    if (!ReadFootSlot(footAudio, foot, current) ||
                        !footmaterial::NeedsWrite(current.container, current.material, container[foot], material[foot]))
                        continue;
                    if (WriteFootSlot(footAudio, foot, container[foot], material[foot]))
                        g_footMaterialDetourWrites.fetch_add(1, std::memory_order_relaxed);
                }
                // foot_material.h kWetnessBase: the recorded wetness of each
                // foot, read by the steps about to play (the puppet's own update
                // set the replay world's).
                if (g_footWetnessReplay.load(std::memory_order_relaxed) && FootProof().wetness)
                {
                    for (std::uint8_t foot = 0; foot < footmaterial::kFeet; ++foot)
                    {
                        float current = 0.0f;
                        if (!GuardedRead(&current, footAudio + footmaterial::WetnessOffset(foot), sizeof(current)) ||
                            !footmaterial::NeedsWetnessWrite(current, material[foot].wetness))
                            continue;
                        const float desired = material[foot].wetness;
                        if (replay::mem::Put(reinterpret_cast<std::uintptr_t>(footAudio + footmaterial::WetnessOffset(foot)),
                                &desired, sizeof(desired)))
                            g_footWetnessDetourWrites.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                // The recorded ground layer weights of each foot, read by the
                // steps about to play (the puppet's own [vtable +0x80] just
                // wrote its own).
                if (g_footLayerReplay.load(std::memory_order_relaxed) && FootProof().layers)
                {
                    for (std::uint8_t foot = 0; foot < footmaterial::kFeet; ++foot)
                    {
                        if (!material[foot].layersKnown)
                            continue;
                        for (std::uint8_t l = 0; l < footmaterial::kLayers; ++l)
                        {
                            float current = 0.0f;
                            const float desired = material[foot].layers[l];
                            if (!GuardedRead(&current, footAudio + footmaterial::LayerOffset(foot, l), sizeof(current)) ||
                                !footmaterial::NeedsLayerWrite(current, desired))
                                continue;
                            if (replay::mem::Put(reinterpret_cast<std::uintptr_t>(footAudio + footmaterial::LayerOffset(foot, l)),
                                    &desired, sizeof(desired)))
                                g_footLayerDetourWrites.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                // foot_material.h FootMix: the recorded volume and filters of
                // each foot, read by the steps about to play (a puppet's own
                // foot-down, silenced at the entry, still ran the hit pattern
                // that writes the slot).
                {
                    for (std::uint8_t foot = 0; foot < footmaterial::kFeet; ++foot)
                    {
                        footmaterial::FootMix current;
                        footmaterial::FootMix desired = mix[foot];
                        float puppetBase = 0.0f;
                        if (desired.known && ReadFootBase(footAudio, puppetBase))
                            desired.volume = footmaterial::SlotVolumeWithBase(desired.volume, drive[foot], puppetBase, true);
                        if (!desired.known || !ReadFootMix(footAudio, foot, current) ||
                            !footmaterial::NeedsMixWrite(current, desired))
                            continue;
                        if (WriteFootMix(footAudio, foot, desired))
                            g_footMixDetourWrites.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                // FootDrive: the foot's locomotion value (variable 0x01401A30 of
                // the step about to play).
                if (g_footImpactReplay.load(std::memory_order_relaxed))
                {
                    for (std::uint8_t foot = 0; foot < footmaterial::kFeet; ++foot)
                        if (drive[foot].impactKnown && WriteFootImpact(footAudio, foot, drive[foot].impact))
                            g_footImpactDetourWrites.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
        }
    }

        void __fastcall MovementUpdateDetour(void* footAudio)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.audio_movement");
            InFlight inFlight;
            // Before the queued steps play (the movement sounds).
            if (footAudio)
            {
                auto* foot = static_cast<std::uint8_t*>(footAudio);
                std::uint8_t feet[footmaterial::kQueueMax] = {};
                const std::uint32_t queued = QueuedFeet(foot, feet);
                // The secondary steps the replay queued on a puppet (second
                // queue +0xB4, count +0xD4) read the same slots when the
                // movement sounds play them.
                std::int32_t secondary = 0;
                if (!queued && g_silencedFeetCount.load(std::memory_order_relaxed) != 0)
                    GuardedRead(&secondary, foot + footevent::kQueueCountOffset, sizeof(secondary));
                if (queued || secondary > 0)
                {
                    if (IsSilencedFoot(footAudio))
                    {
                        ApplyFootMaterials(foot);
                    }
                    else if (queued && g_enabled.load(std::memory_order_relaxed))
                    {
                        SampleFootMaterials(foot, feet, queued);
                    }
                }
            }
            const MovementUpdateFn original = g_originalMovementUpdate.load(std::memory_order_acquire);
            ReplayedSystemScope scope;
            if (original)
                original(footAudio);
        }
    namespace
    {

        // foot_event_core.h: the second queue's entry, detoured. The
        // generator (kGeneratorCall) and the drain (kDrainCall) are told apart
        // by the return address; an entry the engine queued is read back at
        // the index the count had before the call (the store, kEnqueueStore,
        // is the count's only increment). A puppet's own event is refused only while
        // a clip with FSEV plays (the replay queues the recorded ones through
        // the original, PlayFootEvent). No native, no write.
        constexpr std::uint32_t kFootEventRing = 2048;
        Slot<FootEventCall> g_footEvents[kFootEventRing];
        std::atomic<std::uint64_t> g_nextFootEvent{0};
    }
        std::atomic<FootEventFn> g_originalFootEvent{nullptr};
        void* g_footEventTarget = nullptr;
        const char* g_footEventError = "not_installed";
        std::atomic<bool> g_footEventRefuseOwn{false};
    namespace
    {
        std::atomic<std::uint64_t> g_footEventCalls{0};
        std::atomic<std::uint64_t> g_footEventQueued{0};
        std::atomic<std::uint64_t> g_footEventRefused{0};
        std::atomic<std::uint64_t> g_footEventReplayQueued{0};
        std::atomic<std::uint64_t> g_footEventReplayFailed{0};

        void PushFootEvent(const FootEventCall& call)
        {
            replay::ring::Writer<FootEventCall> slot(g_footEvents, kFootEventRing, g_nextFootEvent);
            slot.value = call;
            slot.Publish();
        }
    }

        void __fastcall FootEventDetour(void* footAudio, std::int32_t type, std::uint8_t foot, std::uint8_t spread,
            std::uint8_t direct)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            const FootEventFn original = g_originalFootEvent.load(std::memory_order_acquire);
            g_footEventCalls.fetch_add(1, std::memory_order_relaxed);
            FootEventCall call;
            call.timeUs = QpcUs();
            call.footAudio = reinterpret_cast<std::uint64_t>(footAudio);
            call.argType = type;
            call.argFoot = foot;
            call.spread = spread;
            call.direct = direct;
            call.origin = static_cast<std::uint8_t>(footevent::OriginOf(ToRva(_ReturnAddress()), Callers().footEvent));
            call.silenced = footAudio && g_silencedFeetCount.load(std::memory_order_relaxed) != 0 && IsSilencedFoot(footAudio);
            if (call.silenced && g_footEventRefuseOwn.load(std::memory_order_relaxed))
            {
                call.refused = true;
                g_footEventRefused.fetch_add(1, std::memory_order_relaxed);
                PushFootEvent(call);
                return;
            }
            auto* bytes = static_cast<std::uint8_t*>(footAudio);
            std::int32_t before = -1;
            if (bytes)
                GuardedRead(&before, bytes + footevent::kQueueCountOffset, sizeof(before));
            if (original)
                original(footAudio, type, foot, spread, direct);
            std::int32_t after = -1;
            if (bytes && before >= 0 && before < footevent::kQueueMax &&
                GuardedRead(&after, bytes + footevent::kQueueCountOffset, sizeof(after)) && after == before + 1)
            {
                std::uint8_t raw[footevent::kQueueStride] = {};
                if (GuardedRead(raw, bytes + footevent::kQueueOffset + footevent::kQueueStride * before, sizeof(raw)))
                {
                    call.queued = true;
                    footevent::DecodeQueueEntry(raw, call.type, call.foot);
                    g_footEventQueued.fetch_add(1, std::memory_order_relaxed);
                    // The slot and the rise the sound reads when the queue
                    // plays (a quadruped's drain runs inside the movement
                    // update, right before the movement sounds).
                    FootSlot slot;
                    if (call.foot < footmaterial::kFeet && ReadFootSlot(bytes, call.foot, slot))
                    {
                        call.materialHash = slot.material.hash;
                        call.materialRef = slot.material.ref;
                    }
                    const std::uint64_t component = call.foot < footmaterial::kFeet ? LocomotionComponent(bytes) : 0;
                    call.impactKnown = component &&
                        GuardedRead(&call.impact, reinterpret_cast<const void*>(component + footmaterial::ImpactOffset(call.foot)),
                            sizeof(call.impact)) &&
                        footevent::PlausibleImpact(call.impact);
                }
            }
            PushFootEvent(call);
        }

    void CollectFootstepsSince(std::uint64_t& cursor, std::vector<FootstepCall>& out)
    {
        CollectSince(g_footsteps, kFootstepRing, g_nextFootstep, cursor, out);
    }

    void CollectFootMaterialSamplesSince(std::uint64_t& cursor, std::vector<footmaterial::Sample>& out)
    {
        CollectSince(g_footMaterialSamples, kFootMaterialRing, g_nextFootMaterialSample, cursor, out);
    }
    namespace
    {

        // Script thread only (PlayFootstep, ClearFootMaterials).
        FootMaterialStats g_footMaterialStats;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> g_footContainers; // ref low -> container

        void SetFootMaterialEntry(std::uint64_t footAudio, std::uint8_t foot, std::uint64_t container,
            const footmaterial::Material& material)
        {
            std::uint32_t count = g_footMaterialCount.load(std::memory_order_relaxed);
            FootMaterialEntry* entry = nullptr;
            for (std::uint32_t n = 0; n < count; ++n)
                if (g_footMaterials[n].footAudio.load(std::memory_order_relaxed) == footAudio)
                {
                    entry = &g_footMaterials[n];
                    break;
                }
            if (!entry)
            {
                if (count >= kMaxFootMaterialFeet)
                    return;
                entry = &g_footMaterials[count];
                entry->version.fetch_add(1, std::memory_order_acq_rel); // odd: not readable yet
                for (std::uint8_t f = 0; f < footmaterial::kFeet; ++f)
                {
                    entry->container[f] = 0;
                    entry->material[f] = {};
                    entry->mix[f] = {};
                    entry->drive[f] = {};
                }
                entry->footAudio.store(footAudio, std::memory_order_relaxed);
                entry->version.fetch_add(1, std::memory_order_acq_rel);
                g_footMaterialCount.store(count + 1, std::memory_order_release);
            }
            entry->version.fetch_add(1, std::memory_order_acq_rel);
            std::atomic_thread_fence(std::memory_order_release);
            entry->container[foot] = container;
            // A material without wetness keeps the foot's last one.
            const float keptWetness = entry->material[foot].wetness;
            const footmaterial::Material kept = entry->material[foot];
            entry->material[foot] = material;
            if (!(material.wetness >= 0.0f))
                entry->material[foot].wetness = keptWetness;
            if (!material.layersKnown)
            {
                entry->material[foot].layersKnown = kept.layersKnown;
                std::memcpy(entry->material[foot].layers, kept.layers, sizeof(kept.layers));
            }
            std::atomic_thread_fence(std::memory_order_release);
            entry->version.fetch_add(1, std::memory_order_acq_rel);
        }

        // The wetness alone (a step without a valid material).
        void SetFootWetnessEntry(std::uint64_t footAudio, std::uint8_t foot, float wetness)
        {
            std::uint32_t count = g_footMaterialCount.load(std::memory_order_relaxed);
            for (std::uint32_t n = 0; n < count; ++n)
            {
                FootMaterialEntry& entry = g_footMaterials[n];
                if (entry.footAudio.load(std::memory_order_relaxed) != footAudio)
                    continue;
                entry.version.fetch_add(1, std::memory_order_acq_rel);
                std::atomic_thread_fence(std::memory_order_release);
                entry.material[foot].wetness = wetness;
                std::atomic_thread_fence(std::memory_order_release);
                entry.version.fetch_add(1, std::memory_order_acq_rel);
                return;
            }
            footmaterial::Material material;
            material.wetness = wetness;
            SetFootMaterialEntry(footAudio, foot, 0, material);
        }

        // The mix alone, kept with the foot's material.
        void SetFootMixEntry(std::uint64_t footAudio, std::uint8_t foot, const footmaterial::FootMix& mix,
            const footmaterial::FootDrive& drive)
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                const std::uint32_t count = g_footMaterialCount.load(std::memory_order_relaxed);
                for (std::uint32_t n = 0; n < count; ++n)
                {
                    FootMaterialEntry& entry = g_footMaterials[n];
                    if (entry.footAudio.load(std::memory_order_relaxed) != footAudio)
                        continue;
                    entry.version.fetch_add(1, std::memory_order_acq_rel);
                    std::atomic_thread_fence(std::memory_order_release);
                    entry.mix[foot] = mix;
                    entry.drive[foot] = drive;
                    std::atomic_thread_fence(std::memory_order_release);
                    entry.version.fetch_add(1, std::memory_order_acq_rel);
                    return;
                }
                if (pass)
                    return;
                // No entry yet: create one that carries nothing else (a
                // material without a ref and an unknown wetness are skipped).
                SetFootMaterialEntry(footAudio, foot, 0, footmaterial::Material{});
            }
        }

        // The ground layers alone, kept with the foot's material (the movement
        // update entry writes them).
        void SetFootLayersEntry(std::uint64_t footAudio, std::uint8_t foot, const float (&layers)[footmaterial::kLayers])
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                const std::uint32_t count = g_footMaterialCount.load(std::memory_order_relaxed);
                for (std::uint32_t n = 0; n < count; ++n)
                {
                    FootMaterialEntry& entry = g_footMaterials[n];
                    if (entry.footAudio.load(std::memory_order_relaxed) != footAudio)
                        continue;
                    entry.version.fetch_add(1, std::memory_order_acq_rel);
                    std::atomic_thread_fence(std::memory_order_release);
                    entry.material[foot].layersKnown = true;
                    std::memcpy(entry.material[foot].layers, layers, sizeof(layers));
                    std::atomic_thread_fence(std::memory_order_release);
                    entry.version.fetch_add(1, std::memory_order_acq_rel);
                    return;
                }
                if (pass)
                    return;
                SetFootMaterialEntry(footAudio, foot, 0, footmaterial::Material{});
            }
        }
    }

    void ClearFootMaterials()
    {
        const std::uint32_t count = g_footMaterialCount.load(std::memory_order_relaxed);
        g_footMaterialCount.store(0, std::memory_order_release);
        for (std::uint32_t n = 0; n < count && n < kMaxFootMaterialFeet; ++n)
            g_footMaterials[n].footAudio.store(0, std::memory_order_relaxed);
        g_footContainers.clear();
        g_footMaterialStats = {};
        g_footMaterialDetourWrites.store(0, std::memory_order_relaxed);
        g_footWetnessDetourWrites.store(0, std::memory_order_relaxed);
        g_footMixDetourWrites.store(0, std::memory_order_relaxed);
        g_footImpactDetourWrites.store(0, std::memory_order_relaxed);
        g_footLayerDetourWrites.store(0, std::memory_order_relaxed);
    }

    void SetFootLayerReplay(bool enabled) { g_footLayerReplay.store(enabled, std::memory_order_relaxed); }
    bool FootLayerReplay() { return g_footLayerReplay.load(std::memory_order_relaxed); }

    void SetFootWetnessReplay(bool enabled) { g_footWetnessReplay.store(enabled, std::memory_order_relaxed); }
    bool FootWetnessReplay() { return g_footWetnessReplay.load(std::memory_order_relaxed); }
    void SetFootImpactReplay(bool enabled) { g_footImpactReplay.store(enabled, std::memory_order_relaxed); }

    FootMaterialStats GetFootMaterialStats()
    {
        FootMaterialStats stats = g_footMaterialStats;
        stats.detourWrites = g_footMaterialDetourWrites.load(std::memory_order_relaxed);
        stats.wetDetourWrites = g_footWetnessDetourWrites.load(std::memory_order_relaxed);
        stats.mixDetourWrites = g_footMixDetourWrites.load(std::memory_order_relaxed);
        stats.impactDetourWrites = g_footImpactDetourWrites.load(std::memory_order_relaxed);
        stats.layerDetourWrites = g_footLayerDetourWrites.load(std::memory_order_relaxed);
        stats.samples = g_footMaterialSampled.load(std::memory_order_relaxed);
        return stats;
    }

    std::uint64_t FootstepAudio(void* pedEntity)
    {
        const std::uint64_t holder = PedAudioHolder(pedEntity);
        return holder ? holder + kFootAudioOffset : 0;
    }
    namespace
    {

        bool GuardedFootstep(std::uintptr_t play, std::uint8_t* footAudio, const FootstepCall& step, std::uint32_t& code)
        {
            return engine::Call(engine::kFootstep, play, footAudio, step.eventId, step.arg3, step.foot, step.arg5, step.flag,
                       step.arg7, step.arg8) ||
                engine::Failed(code);
        }

        // Guarded write on the puppet's own foot audio (vtable checked) of the
        // container the engine's own lookup returns for the recorded ref
        // (kRefContainer, as IndirectRefState). `stats` lets the secondary
        // steps (PlayFootEvent) keep their own counters.
        void SetPuppetFootMaterial(std::uint8_t* footAudio, const FootstepCall& step,
            FootMaterialStats& stats = g_footMaterialStats)
        {
            ++stats.requested;
            std::uint64_t vtable = 0;
            if (step.foot >= footmaterial::kFeet || !GuardedRead(&vtable, footAudio, 8) || !IsFootAudio(vtable))
            {
                ++stats.badFoot;
                return;
            }
            static const std::uintptr_t lookup = sig::Find(kRefContainer);
            if (!lookup)
            {
                ++stats.noContainer;
                return;
            }
            std::uint64_t container = 0;
            const auto cached = std::find_if(g_footContainers.begin(), g_footContainers.end(),
                [&](const auto& entry) { return entry.first == step.materialRef; });
            if (cached != g_footContainers.end())
            {
                container = cached->second;
            }
            else
            {
                void* found = nullptr;
                if (!GuardedRefContainer(lookup, step.materialRef, found))
                    found = nullptr;
                container = reinterpret_cast<std::uint64_t>(found);
                if (g_footContainers.size() < 512)
                    g_footContainers.push_back({step.materialRef, container});
            }
            if (!container)
            {
                ++stats.noContainer;
                return;
            }
            footmaterial::Material material;
            material.hash = step.materialHash;
            material.ref = step.materialRef;
            SetFootMaterialEntry(reinterpret_cast<std::uint64_t>(footAudio), step.foot, container, material);
            FootSlot current;
            if (!ReadFootSlot(footAudio, step.foot, current))
            {
                ++stats.badFoot;
                return;
            }
            if (!footmaterial::NeedsWrite(current.container, current.material, container, material))
            {
                ++stats.same;
                return;
            }
            if (WriteFootSlot(footAudio, step.foot, container, material))
                ++stats.written;
            else
                ++stats.badFoot;
        }

        // foot_material.h FootMix: guarded write of the recorded per-foot
        // volume and filters on the puppet's own foot audio (vtable checked,
        // as SetPuppetFootWetness), kept in the table for the movement update
        // entry.
        void SetPuppetFootMix(std::uint8_t* footAudio, const FootstepCall& step)
        {
            FootMaterialStats& stats = g_footMaterialStats;
            ++stats.mixRequested;
            std::uint64_t vtable = 0;
            if (step.foot >= footmaterial::kFeet || !GuardedRead(&vtable, footAudio, 8) || !IsFootAudio(vtable))
            {
                ++stats.mixBadFoot;
                return;
            }
            footmaterial::FootMix mix = step.mix;
            SetFootMixEntry(reinterpret_cast<std::uint64_t>(footAudio), step.foot, mix, step.drive);
            if (g_footImpactReplay.load(std::memory_order_relaxed) && step.drive.impactKnown &&
                WriteFootImpact(footAudio, step.foot, step.drive.impact))
                ++stats.impactWritten;
            if (!mix.known)
                return;
            float puppetBase = 0.0f;
            if (ReadFootBase(footAudio, puppetBase) && step.drive.baseKnown)
            {
                mix.volume = footmaterial::SlotVolumeWithBase(mix.volume, step.drive, puppetBase, true);
                ++stats.baseApplied;
            }
            footmaterial::FootMix current;
            if (!ReadFootMix(footAudio, step.foot, current))
            {
                ++stats.mixBadFoot;
                return;
            }
            if (!footmaterial::NeedsMixWrite(current, mix))
            {
                ++stats.mixSame;
                return;
            }
            if (WriteFootMix(footAudio, step.foot, mix))
                ++stats.mixWritten;
            else
                ++stats.mixBadFoot;
        }

        void SetPuppetFootWetness(std::uint8_t* footAudio, const FootstepCall& step)
        {
            FootMaterialStats& stats = g_footMaterialStats;
            ++stats.wetRequested;
            if (footmaterial::WetStep(step.wetness))
                ++stats.wetWet;
            std::uint64_t vtable = 0;
            if (step.foot >= footmaterial::kFeet || !GuardedRead(&vtable, footAudio, 8) || !IsFootAudio(vtable))
            {
                ++stats.wetBadFoot;
                return;
            }
            SetFootWetnessEntry(reinterpret_cast<std::uint64_t>(footAudio), step.foot, step.wetness);
            if (!FootProof().wetness)
            {
                ++stats.wetBadFoot;
                return;
            }
            float current = 0.0f;
            if (!GuardedRead(&current, footAudio + footmaterial::WetnessOffset(step.foot), sizeof(current)))
            {
                ++stats.wetBadFoot;
                return;
            }
            if (!footmaterial::NeedsWetnessWrite(current, step.wetness))
            {
                ++stats.wetSame;
                return;
            }
            const float desired = step.wetness;
            if (replay::mem::Put(reinterpret_cast<std::uintptr_t>(footAudio + footmaterial::WetnessOffset(step.foot)),
                    &desired, sizeof(desired)))
                ++stats.wetWritten;
            else
                ++stats.wetBadFoot;
        }

        // foot_material.h kLayerBase: guarded write of the recorded ground
        // layer weights on the puppet's own foot audio (vtable checked, as
        // SetPuppetFootWetness), kept in the table for the movement update
        // entry, where the step reads them.
        void SetPuppetFootLayers(std::uint8_t* footAudio, const FootstepCall& step)
        {
            FootMaterialStats& stats = g_footMaterialStats;
            ++stats.layerRequested;
            std::uint64_t vtable = 0;
            if (step.foot >= footmaterial::kFeet || !GuardedRead(&vtable, footAudio, 8) || !IsFootAudio(vtable))
            {
                ++stats.layerBadFoot;
                return;
            }
            SetFootLayersEntry(reinterpret_cast<std::uint64_t>(footAudio), step.foot, step.layers);
            if (!FootProof().layers)
            {
                ++stats.layerBadFoot;
                return;
            }
            bool wrote = false, fault = false;
            for (std::uint8_t l = 0; l < footmaterial::kLayers; ++l)
            {
                float current = 0.0f;
                if (!GuardedRead(&current, footAudio + footmaterial::LayerOffset(step.foot, l), sizeof(current)))
                {
                    fault = true;
                    break;
                }
                if (!footmaterial::NeedsLayerWrite(current, step.layers[l]))
                    continue;
                const float desired = step.layers[l];
                if (replay::mem::Put(reinterpret_cast<std::uintptr_t>(footAudio + footmaterial::LayerOffset(step.foot, l)),
                        &desired, sizeof(desired)))
                    wrote = true;
                else
                    fault = true;
            }
            ++(fault ? stats.layerBadFoot : wrote ? stats.layerWritten : stats.layerSame);
        }
    }

    bool PlayFootstep(void* pedEntity, const FootstepCall& step, std::string& error)
    {
        auto play = reinterpret_cast<std::uintptr_t>(g_originalFootstep.load(std::memory_order_acquire));
        if (!play)
        {
            play = sig::Find(kFootstep);
            if (!play)
            {
                error = "footstep_signature_mismatch";
                return false;
            }
        }
        for (const sig::Signature* site : kFootstepArgs)
        {
            if (!sig::Find(*site))
            {
                error = "footstep_signature_mismatch";
                return false;
            }
        }
        auto* footAudio = reinterpret_cast<std::uint8_t*>(FootstepAudio(pedEntity));
        std::uint8_t enabled = 0;
        if (!footAudio || !GuardedRead(&enabled, footAudio + 0x24, 1))
        {
            error = "no_footstep_audio";
            return false;
        }
        // The foot-down handler only queues when this is set (its own test).
        if (!enabled)
        {
            // Why (the activation): a pending component is activated only while its
            // ped audio entity ([footAudio+0x10]) is registered (+0x10 !=
            // 0xFFFF).
            error = "footstep_audio_disabled";
            std::uint64_t owner = 0;
            std::uint16_t slot = 0;
            std::uint32_t pending = 0;
            if (GuardedRead(&owner, footAudio + 0x10, 8) && owner &&
                GuardedRead(&slot, reinterpret_cast<const void*>(owner + 0x10), 2) &&
                GuardedRead(&pending, footAudio + 0x20, 4))
            {
                error += std::string(slot == 0xFFFF ? ":unregistered" : ":registered") + ((pending & 2) ? ":pending" : "");
                // The holder's audio LOD against the foot's max LOD
                // (the component's test: active only while LOD <= max).
                std::uint8_t lod = 0xFF, maxLod = 0xFF;
                if (slot != 0xFFFF && GuardedRead(&lod, reinterpret_cast<const void*>(owner + 0x45E8), 1) &&
                    GuardedRead(&maxLod, footAudio + 0x28, 1) && lod != 0xFF && maxLod != 0xFF && lod > maxLod)
                    error += ":lod" + std::to_string(lod) + "_over_max" + std::to_string(maxLod);
            }
            return false;
        }
        // foot_material.h: the recorded standing material of that foot on the
        // puppet's step container slot; the movement update detour sets it
        // again before the step plays (the puppet's own ground probe
        // refreshes the slot).
        if (footmaterial::ValidRef(step.materialRef))
            SetPuppetFootMaterial(footAudio, step);
        if (g_footWetnessReplay.load(std::memory_order_relaxed) && step.wetness >= 0.0f)
            SetPuppetFootWetness(footAudio, step);
        if (step.mix.known || (g_footImpactReplay.load(std::memory_order_relaxed) && step.drive.impactKnown))
            SetPuppetFootMix(footAudio, step);
        if (g_footLayerReplay.load(std::memory_order_relaxed) && step.layersKnown)
            SetPuppetFootLayers(footAudio, step);
        std::uint32_t code = 0;
        if (!GuardedFootstep(play, footAudio, step, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        error = "queued";
        return true;
    }
    namespace
    {

    // foot_event_core.h: the second queue of the foot audio, script thread
    // side.
        FootMaterialStats g_footEventMaterialStats; // script thread only (PlayFootEvent)

        // The recorded rise of one foot alone, kept for the movement update
        // entry (ApplyFootMaterials writes it before the queues play); the
        // foot's base and mix stay as PlayFootstep left them.
        void SetFootImpactEntry(std::uint64_t footAudio, std::uint8_t foot, float impact)
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                const std::uint32_t count = g_footMaterialCount.load(std::memory_order_relaxed);
                for (std::uint32_t n = 0; n < count; ++n)
                {
                    FootMaterialEntry& entry = g_footMaterials[n];
                    if (entry.footAudio.load(std::memory_order_relaxed) != footAudio)
                        continue;
                    entry.version.fetch_add(1, std::memory_order_acq_rel);
                    std::atomic_thread_fence(std::memory_order_release);
                    entry.drive[foot].impactKnown = true;
                    entry.drive[foot].impact = impact;
                    std::atomic_thread_fence(std::memory_order_release);
                    entry.version.fetch_add(1, std::memory_order_acq_rel);
                    return;
                }
                if (pass)
                    return;
                SetFootMaterialEntry(footAudio, foot, 0, footmaterial::Material{});
            }
        }

        // The drain's own call: (foot audio, type, foot, spread 0, direct 1).
        bool GuardedFootEvent(std::uintptr_t play, std::uint8_t* footAudio, std::int32_t type, std::uint8_t foot,
            std::uint32_t& code)
        {
            return engine::Call(engine::kFootEventEnqueue, play, footAudio, type, foot, 0, 1) || engine::Failed(code);
        }
    }

    bool PlayFootEvent(void* pedEntity, std::int32_t type, std::uint8_t foot, std::uint32_t materialHash,
        std::uint32_t materialRef, bool impactKnown, float impact, std::string& status)
    {
        using footevent::PlayStatus;
        const auto fail = [&status](PlayStatus s) {
            status = footevent::PlayStatusName(s);
            g_footEventReplayFailed.fetch_add(1, std::memory_order_relaxed);
            return false;
        };
        if (!footevent::ValidType(type) || foot >= footevent::kPlayedFeet)
            return fail(PlayStatus::BadEvent);
        auto play = reinterpret_cast<std::uintptr_t>(g_originalFootEvent.load(std::memory_order_acquire));
        if (!play)
        {
            // Not detoured (the install failed on a site): the entry itself,
            // found with its prologue and its store.
            if (!sig::Find(footevent::kEnqueueStore))
                return fail(PlayStatus::Signature);
            play = sig::Find(footevent::kEnqueue);
            if (!play)
                return fail(PlayStatus::Signature);
        }
        auto* footAudio = reinterpret_cast<std::uint8_t*>(FootstepAudio(pedEntity));
        std::uint64_t vtable = 0;
        if (!footAudio || !GuardedRead(&vtable, footAudio, 8) || !IsFootAudio(vtable))
            return fail(PlayStatus::NoAudio);
        std::uint8_t enabled = 0;
        std::uint8_t blocked = 0;
        std::int32_t before = -1;
        if (!GuardedRead(&enabled, footAudio + footevent::kEnabledOffset, 1) ||
            !GuardedRead(&blocked, footAudio + footevent::kBlockedOffset, 1) ||
            !GuardedRead(&before, footAudio + footevent::kQueueCountOffset, sizeof(before)) || before < 0 ||
            before > footevent::kQueueMax)
            return fail(PlayStatus::NoAudio);
        if (!enabled)
            return fail(PlayStatus::Disabled);
        if (blocked)
            return fail(PlayStatus::Blocked);
        if (before >= footevent::kQueueMax)
            return fail(PlayStatus::QueueFull);
        // The recorded container of that foot's slot (the secondary step
        // reads it when the queue plays) and the recorded rise, as
        // PlayFootstep does for a main step; the movement update entry sets
        // both again (MovementUpdateDetour, the second queue counted).
        if (footmaterial::ValidRef(materialRef))
        {
            FootstepCall step;
            step.foot = foot;
            step.materialHash = materialHash;
            step.materialRef = materialRef;
            SetPuppetFootMaterial(footAudio, step, g_footEventMaterialStats);
        }
        if (impactKnown && footevent::PlausibleImpact(impact) && g_footImpactReplay.load(std::memory_order_relaxed))
        {
            SetFootImpactEntry(reinterpret_cast<std::uint64_t>(footAudio), foot, impact);
            WriteFootImpact(footAudio, foot, impact);
        }
        std::uint32_t code = 0;
        if (!GuardedFootEvent(play, footAudio, type, foot, code))
        {
            g_footEventReplayFailed.fetch_add(1, std::memory_order_relaxed);
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            status = text;
            return false;
        }
        std::int32_t after = -1;
        if (!GuardedRead(&after, footAudio + footevent::kQueueCountOffset, sizeof(after)) || after != before + 1)
            return fail(PlayStatus::NotQueued);
        g_footEventReplayQueued.fetch_add(1, std::memory_order_relaxed);
        status = footevent::PlayStatusName(PlayStatus::Queued);
        return true;
    }

    void CollectFootEventsSince(std::uint64_t& cursor, std::vector<FootEventCall>& out)
    {
        CollectSince(g_footEvents, kFootEventRing, g_nextFootEvent, cursor, out);
    }

    void SetFootEventRefuseOwn(bool on)
    {
        g_footEventRefuseOwn.store(on && g_footEventTarget != nullptr, std::memory_order_relaxed);
    }

    FootEventStats FootEventSnapshot()
    {
        FootEventStats stats;
        stats.installed = g_footEventTarget != nullptr;
        stats.error = g_footEventError ? g_footEventError : "";
        stats.calls = g_footEventCalls.load(std::memory_order_relaxed);
        stats.queued = g_footEventQueued.load(std::memory_order_relaxed);
        stats.refused = g_footEventRefused.load(std::memory_order_relaxed);
        stats.replayQueued = g_footEventReplayQueued.load(std::memory_order_relaxed);
        stats.replayFailed = g_footEventReplayFailed.load(std::memory_order_relaxed);
        return stats;
    }

    bool ReadMotionState(void* pedEntity, footevent::MotionState& out)
    {
        using namespace footevent;
        out = MotionState{};
        const std::uint64_t row = PedComponentRow(pedEntity);
        std::uint64_t component = 0;
        if (!row || !ReadU64(row + 0x88, component) || !(component & ~1ull))
            return false;
        component &= ~1ull;
        // The component's ped, as the engine reads it ([C +8] & ~0xF).
        std::uint64_t owner = 0;
        if (!ReadU64(component + 8, owner) || (owner & ~0xFull) != reinterpret_cast<std::uint64_t>(pedEntity))
            return false;
        const auto* c = reinterpret_cast<const std::uint8_t*>(component);
        if (!GuardedRead(&out.speed, c + kSpeedOffset, sizeof(out.speed)) ||
            !GuardedRead(&out.gait, c + kGaitOffset, sizeof(out.gait)) ||
            !GuardedRead(&out.gaitLoaded, c + kGaitLoadedOffset, sizeof(out.gaitLoaded)))
            return false;
        std::uint64_t table = 0;
        out.tableKnown = ReadU64(component + kGaitTableOffset, table) && table &&
            GuardedRead(out.t, reinterpret_cast<const void*>(table + 4), sizeof(out.t));
        float thrBase = 0.0f, thrScale = 0.0f, thrInput = 0.0f;
        out.thrKnown = GuardedRead(&thrBase, c + kThresholdBaseOffset, 4) && GuardedRead(&thrScale, c + kThresholdScaleOffset, 4) &&
            GuardedRead(&thrInput, c + kThresholdInputOffset, 4);
        if (out.thrKnown)
            out.thr = thrBase + thrScale * thrInput;
        bool feet = true;
        for (std::uint8_t f = 0; f < 4 && feet; ++f)
        {
            const std::uintptr_t at = static_cast<std::uintptr_t>(f) * kFootStride;
            feet = GuardedRead(&out.height[f], c + kFootHeightOffset + at, 4) && GuardedRead(&out.rise[f], c + kFootRiseOffset + at, 4) &&
                GuardedRead(&out.last[f], c + kFootStateOffset + at, 4);
        }
        out.feetKnown = feet;
        GuardedRead(&out.contactsA, c + kContactsAOffset, 4);
        GuardedRead(&out.contactsB, c + kContactsBOffset, 4);
        GuardedRead(&out.b63e, c + kByte63EOffset, 1);
        std::uint64_t link = 0;
        out.mountLink = ReadU64(row + kRowMountLinkSlot, link) && (link & ~1ull) != 0;
        std::uint64_t motion = 0;
        if (ReadU64(row + kRowMotionSlot, motion) && (motion & ~1ull))
        {
            motion &= ~1ull;
            out.d0Known = GuardedRead(&out.d0State, reinterpret_cast<const void*>(motion + kMotionStateOffset), 4) &&
                GuardedRead(&out.d0Mbr, reinterpret_cast<const void*>(motion + kMotionMbrOffset), 4);
        }
        out.read = true;
        return true;
    }
    namespace
    {

    // foot_material.h MovementTermCorrection: the movement volume
    // (foot audio, limb data, foot, f32, out low-pass), detoured: the original
    // runs, then the -2 dB term it applies to a puppet because the hidden live
    // player is on foot is given back when the recorded state would not have
    // applied it (the player's own puppet; any puppet while the recorded
    // player rides). The predicate pattern of the mount detour: no write, no
    // call but the original and the mount predicate's original.
        // The player term, f32 -2.0 in the exe, as the volume reads it.
        const sig::Signature kMovementTermRead{"audio_movement_term_read", kMovementVolume, sig::Offset{0x11F},
            "F3 0F 10 35 ?? ?? ?? ??", 0x31816E3};
        const sig::Signature kMovementTerm{"audio_movement_term", kMovementTermRead, sig::Rel32{0x4, 0x8},
            "", 0x3E67E5C};
        using MovementVolumeFn = float(__fastcall*)(void*, void*, std::uint8_t, float, std::uint32_t*);
        std::atomic<MovementVolumeFn> g_originalMovementVolume{nullptr};
    }
        void* g_movementVolumeTarget = nullptr;
    namespace
    {
        std::atomic<bool> g_movementTermEnabled{false};
        std::atomic<bool> g_movementLivePlayerMounted{false};
        std::atomic<std::uint64_t> g_movementTermCalls{0};
        std::atomic<std::uint64_t> g_movementTermGiven{0};
        std::atomic<std::uint64_t> g_movementTermGivenPlayer{0};
        float g_movementTerm = -2.0f;

        bool RecordedPlayerRides()
        {
            for (const std::atomic<std::uint64_t>& published : g_mountAudioComponents)
                if (published.load(std::memory_order_acquire))
                    return true;
            return false;
        }

        // foot_event_core.h MovementWindow: the evidence ring.
        constexpr std::uint32_t kMovementDetailRing = 2048;
        Slot<MovementDetailCall> g_movementDetail[kMovementDetailRing];
        std::atomic<std::uint64_t> g_nextMovementDetail{0};
    }
        std::atomic<bool> g_movementDetailOn{false};
    namespace
    {

        void NoteMovementDetail(void* foot, std::uint8_t footIndex, float value, float result)
        {
            MovementDetailCall call;
            call.timeUs = QpcUs();
            call.footAudio = reinterpret_cast<std::uint64_t>(foot);
            call.foot = footIndex;
            call.value = value;
            call.result = result;
            const auto* bytes = static_cast<const std::uint8_t*>(foot);
            std::uint64_t holder = 0;
            std::uint8_t gate = 0;
            if (GuardedRead(&holder, bytes + 0x10, 8) && holder &&
                GuardedRead(&gate, reinterpret_cast<const void*>(holder + footevent::kMovementLimbGateOffset), 1))
                call.gate = static_cast<std::int8_t>(gate ? 1 : 0);
            call.f504Known = GuardedRead(&call.f504, bytes + footevent::kMovementF504Offset, sizeof(call.f504));
            replay::ring::Writer<MovementDetailCall> slot(g_movementDetail, kMovementDetailRing, g_nextMovementDetail);
            slot.value = call;
            slot.Publish();
        }

        // foot_movement_core.h: the recorded results the script thread
        // publishes for the puppets' feet at the playhead (seqlock per entry;
        // the detour only reads).
        constexpr std::size_t kRecordedMovementEntries = 32;
        struct RecordedMovementEntry
        {
            std::atomic<std::uint32_t> sequence{0}; // odd while written
            std::atomic<std::uint64_t> ped{0};
            std::atomic<std::uint32_t> mask{0};
            std::atomic<std::uint32_t> bits[footmovement::kMaxFoot] = {};
        };
        RecordedMovementEntry g_recordedMovement[kRecordedMovementEntries];
        std::atomic<bool> g_movementReplayOn{false};
        std::atomic<std::uint64_t> g_movementReplayGiven{0};
        std::atomic<std::uint64_t> g_movementReplayUnmatched{0};

        bool RecordedMovementFor(std::uint64_t ped, std::uint8_t foot, float& out)
        {
            if (foot >= footmovement::kMaxFoot)
                return false;
            for (RecordedMovementEntry& e : g_recordedMovement)
            {
                if (e.ped.load(std::memory_order_acquire) != ped)
                    continue;
                const std::uint32_t before = e.sequence.load(std::memory_order_acquire);
                if (before & 1u)
                    return false;
                const std::uint32_t mask = e.mask.load(std::memory_order_acquire);
                const std::uint32_t bits = e.bits[foot].load(std::memory_order_acquire);
                if (e.sequence.load(std::memory_order_acquire) != before || e.ped.load(std::memory_order_acquire) != ped)
                    return false;
                if (!(mask & (1u << foot)))
                    return false;
                std::memcpy(&out, &bits, sizeof(out));
                return true;
            }
            return false;
        }

        float __fastcall MovementVolumeDetour(void* foot, void* limb, std::uint8_t footIndex, float value, std::uint32_t* lowPass)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            const MovementVolumeFn original = g_originalMovementVolume.load(std::memory_order_acquire);
            float result = original ? original(foot, limb, footIndex, value, lowPass) : 0.0f;
            if (foot && g_movementDetailOn.load(std::memory_order_relaxed)) // evidence
                NoteMovementDetail(foot, footIndex, value, result);
            const bool replayOn = g_movementReplayOn.load(std::memory_order_relaxed);
            const bool termOn = g_movementTermEnabled.load(std::memory_order_relaxed);
            if ((!termOn && !replayOn) || !foot)
                return result;
            if (termOn)
                g_movementTermCalls.fetch_add(1, std::memory_order_relaxed);
            footmaterial::MovementTermState state;
            state.enabled = true;
            state.puppetFoot = IsSilencedFoot(foot);
            if (!state.puppetFoot)
                return result;
            std::uint64_t ped = 0;
            if (!ReadU64(reinterpret_cast<std::uint64_t>(foot) + 0x18, ped) || !ped)
                return result;
            if (replayOn)
            {
                // The live step's result whole (every term in it), no
                // correction on top.
                float recorded = 0.0f;
                const bool matched = RecordedMovementFor(ped, footIndex, recorded);
                if (footmovement::UseRecorded(replayOn, matched))
                {
                    g_movementReplayGiven.fetch_add(1, std::memory_order_relaxed);
                    return recorded;
                }
                g_movementReplayUnmatched.fetch_add(1, std::memory_order_relaxed);
            }
            if (!termOn)
                return result;
            const std::uint64_t row = PedComponentRow(reinterpret_cast<void*>(ped));
            if (!row)
                return result;
            std::uint64_t own = 0;
            std::uint8_t flags = 0;
            if (!ReadU64(row + kPlayerAudioComponentSlot, own) || !(own & ~1ull) ||
                !GuardedRead(&flags, reinterpret_cast<const void*>((own & ~1ull) + kPlayerAudioFlags), 1))
                return result;
            state.ownerPlayerFlag = (flags & 1) == 0 && (flags & 2) != 0;
            std::uint64_t link = 0;
            if (ReadU64(row + kMountLinkSlot, link) && (link & ~1ull))
            {
                const std::uint64_t component = link & ~1ull;
                if (MountAudioPublished(component))
                    state.riderIsPlayer = true;
                else if (const MountAudioPredicateFn predicate = g_originalMountAudio.load(std::memory_order_acquire))
                    state.riderIsPlayer = predicate(reinterpret_cast<const void*>(component));
            }
            state.livePlayerMounted = g_movementLivePlayerMounted.load(std::memory_order_relaxed);
            state.ownerIsPlayerPuppet = PlayerAudioPublished(ped);
            state.recordedPlayerMounted = RecordedPlayerRides();
            const float correction = footmaterial::MovementTermCorrection(state, g_movementTerm);
            if (correction != 0.0f)
            {
                g_movementTermGiven.fetch_add(1, std::memory_order_relaxed);
                if (state.ownerIsPlayerPuppet)
                    g_movementTermGivenPlayer.fetch_add(1, std::memory_order_relaxed);
                result += correction;
            }
            return result;
        }
    }

    bool InstallMovementVolume(std::string& error)
    {
        if (g_movementVolumeTarget)
            return true;
        if (!g_base)
            g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t termAt = sig::Find(kMovementTerm);
        float term = 0.0f;
        if (!termAt || !GuardedRead(&term, reinterpret_cast<const void*>(termAt), sizeof(term)) || term != -2.0f)
        {
            error += "movement_volume_term_mismatch;";
            return false;
        }
        g_movementTerm = term;
        return Hook(kMovementVolume, reinterpret_cast<void*>(&MovementVolumeDetour), g_originalMovementVolume,
            g_movementVolumeTarget, "movement_volume", error);
    }

    void SetMovementTermState(bool enabled, bool livePlayerMounted)
    {
        g_movementTermEnabled.store(enabled && g_movementVolumeTarget, std::memory_order_relaxed);
        g_movementLivePlayerMounted.store(livePlayerMounted, std::memory_order_relaxed);
    }

    void SetMovementDetail(bool on) { g_movementDetailOn.store(on && g_movementVolumeTarget, std::memory_order_relaxed); }

    // The recorded movement results of the puppets' feet.
    void SetMovementReplay(bool on) { g_movementReplayOn.store(on && g_movementVolumeTarget, std::memory_order_relaxed); }

    void PublishRecordedMovement(std::size_t index, std::uint64_t ped, std::uint32_t mask, const float* values)
    {
        if (index >= kRecordedMovementEntries)
            return;
        RecordedMovementEntry& e = g_recordedMovement[index];
        const std::uint32_t seq = e.sequence.load(std::memory_order_relaxed);
        e.sequence.store(seq | 1u, std::memory_order_release);
        e.ped.store(ped, std::memory_order_release);
        e.mask.store(ped ? mask : 0, std::memory_order_release);
        for (std::size_t foot = 0; foot < footmovement::kMaxFoot; ++foot)
        {
            std::uint32_t bits = 0;
            if (values)
                std::memcpy(&bits, &values[foot], sizeof(bits));
            e.bits[foot].store(bits, std::memory_order_release);
        }
        e.sequence.store((seq | 1u) + 1u, std::memory_order_release);
    }

    std::size_t RecordedMovementSlots() { return kRecordedMovementEntries; }

    MovementReplayStats MovementReplaySnapshot()
    {
        MovementReplayStats s;
        s.given = g_movementReplayGiven.load(std::memory_order_relaxed);
        s.unmatched = g_movementReplayUnmatched.load(std::memory_order_relaxed);
        s.on = g_movementReplayOn.load(std::memory_order_relaxed);
        return s;
    }

    void CollectMovementDetailSince(std::uint64_t& cursor, std::vector<MovementDetailCall>& out)
    {
        CollectSince(g_movementDetail, kMovementDetailRing, g_nextMovementDetail, cursor, out);
    }

    MovementTermStats MovementTermSnapshot()
    {
        MovementTermStats stats;
        stats.installed = g_movementVolumeTarget != nullptr;
        stats.calls = g_movementTermCalls.load(std::memory_order_relaxed);
        stats.given = g_movementTermGiven.load(std::memory_order_relaxed);
        stats.givenPlayer = g_movementTermGivenPlayer.load(std::memory_order_relaxed);
        return stats;
    }
}
