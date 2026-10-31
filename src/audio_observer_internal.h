// audio_observer_internal.h: the state and helpers the audio_observer*.cpp files share.
// Internal to those files, not a public header.
#pragma once

#include <intrin.h>
#include "guarded_memory.h"
#include "engine_calls.h"
#include "engine_sigs.h"
#include "mod_clock.h"
#include "audio_observer.h"
#include "speech_slots.h"
#include "speech_conversation.h"
#include "mount_audio.h"
#include "puppet_audio_rule.h"
#include "profiler.h"
#include "sound_generic.h"
#include "sound_owner_table.h" // PacketLives at the ports
#include "vehicle_sound_rule.h"
#include "soundfx_probe.h"
#include "log_policy.h" // Verbose() for the sound callers
#include "sound_check_core.h" // KnownImageAudio, CallSites
#include "sound_world_core.h" // the audio update
#include "shot_audio_delay.h" // FirePlay
#include "entity_query.h"     // the ped audio vtables
#include "game_sig.h"

#include <windows.h>

#include <MinHook.h>
#include "hook_batch.h"
#include "module_lifetime.h"
#include "event_ring.h"

#include <algorithm>
#include <unordered_map>
#include <vector>
#include <cmath> // std::isfinite
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace replay::audio
{

    using enginesig::kAudioCollisionSite;
    using enginesig::kAudioMeleeBody;
    using enginesig::kAudioMeleeImpact;
    using enginesig::kAudioPersistentCreate;
    using enginesig::kAudioStepSound;
    using enginesig::kMovementVolume;

        namespace sig = replay::sig;
        extern const sig::Signature kSoundParamsCtor;
        extern const sig::Signature kSpeechSay;
        // Internal speech params (filled from the script params by the
        // native path): voice hash, partial context hash, variation.
        constexpr std::size_t kParamsVoice = 0x14C;
        constexpr std::size_t kParamsContext = 0x150;
        constexpr std::size_t kParamsVariation = 0x158;
        extern const sig::Signature kFireEnqueue;
        using FireEnqueueFn = void(__fastcall*)(const void*);

        // Melee impact audio entries (engine_sigs.h).
        using MeleeImpactFn = void(__fastcall*)(void*, void*);
        extern const sig::Signature kSoundRequest;
        using SoundRequestFn = bool(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        extern const sig::Signature kSoundParamsRelease;
        constexpr std::uint32_t kSoundRequestRing = 1024;
        extern const sig::Signature kStaticCollisionAudio;
        using PersistentCreateFn = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        extern const sig::Signature kPersistentByRef;
        extern const sig::Signature kPersistentByHash;
        using PersistentByRefFn = void(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        using PersistentByHashFn = void(__fastcall*)(std::uint64_t, std::uint32_t, std::uint64_t, std::uint64_t);
        // The peds' persistent sounds come this way too.
        constexpr std::uint32_t kEntitySoundRing = 1024;
        extern const sig::Signature kStopAndForget;
        using StopAndForgetFn = void(__fastcall*)(std::uint64_t, std::uint8_t);
        extern const sig::Signature kCreateAndPlayPublic;

        using SoundParamsCtorFn = void*(__fastcall*)(void*);
        extern const sig::Signature kSpeechScopeRelease;
        constexpr std::uint32_t kCoreRing = 512;
        extern const sig::Signature kSpeechLineStop;
        using SpeechLineStopFn = void(__fastcall*)(void*, std::uint32_t);
        constexpr std::uint32_t kStopRing = 256;
        constexpr std::uint32_t kStartRing = 256;
        extern const sig::Signature kFootstep;
        using FootstepFn = void(__fastcall*)(void*, std::int32_t, std::uint32_t, std::uint8_t,
            std::uint8_t, std::uint8_t, std::uint32_t, std::uint8_t);
        // Ped movement audio update (foot_event_core.h kMovementUpdate): one
        // argument, the ped's foot audio (holder +0x1560, [+0x18] = ped).
        // Plays the queued footsteps (the second queue) and the movement
        // sounds (scuffs, hooves) from the ped's own animation, which a puppet
        // regenerates by itself.
        using MovementUpdateFn = void(__fastcall*)(void*);
        constexpr std::uintptr_t kFootAudioOffset = 0x1560;
        // The second queue's entry and its sites (the signatures live in
        // foot_event_core.h, where the tests check them).
        using FootEventFn = void(__fastcall*)(void*, std::int32_t, std::uint8_t, std::uint8_t, std::uint8_t);
        constexpr std::uint32_t kExplosionRing = 256;
        constexpr std::uint32_t kAnimRing = 1024;

        // The rings' slots and protocol (event_ring.h): writers claim and
        // publish, readers stop at a ticket not yet published.
        template <typename T>
        using Slot = replay::ring::Slot<T>;
        extern Slot<SpeechCoreCall> g_core[kCoreRing];
        extern std::atomic<std::uint64_t> g_nextCore;
        extern Slot<SpeechLineStop> g_stops[kStopRing];
        extern std::atomic<std::uint64_t> g_nextStop;
        // The puppets' speech entities and their own core calls
        // (puppet_audio_rule.h RefusePuppetOwnSpeech).
        constexpr std::size_t kMaxPuppetSpeech = 256;
        extern std::atomic<std::uint64_t> g_puppetSpeech[kMaxPuppetSpeech];
        extern std::atomic<std::uint32_t> g_puppetSpeechOwner[kMaxPuppetSpeech];
        extern std::atomic<std::uint32_t> g_puppetSpeechCount;
        extern std::atomic<bool> g_puppetOwnSpeechBlock;
        extern std::atomic<std::uintptr_t> g_ownSpeechRelease;
        extern const char* g_ownSpeechError;
        extern std::atomic<std::uint64_t> g_puppetOwnSpeechSeen;
        extern std::atomic<std::uint64_t> g_puppetOwnSpeechRefused;
        extern std::atomic<std::uint64_t> g_worldSpeechDuringPlayback;
        extern std::atomic<std::uint64_t> g_worldSpeechQueuedDuringPlayback;
        constexpr std::uint32_t kOwnSpeechRing = 512;
        extern Slot<PuppetOwnSpeech> g_ownSpeech[kOwnSpeechRing];
        extern std::atomic<std::uint64_t> g_nextOwnSpeech;
        bool PuppetSpeechOwner(std::uint64_t speech, std::uint32_t& owner);
        extern std::atomic<SpeechLineStopFn> g_originalStop;
        extern Slot<SpeechLineStart> g_starts[kStartRing];
        extern std::atomic<std::uint64_t> g_nextStart;
        extern void* g_audibleTarget;
        extern Slot<ExplosionAudioCall> g_explosions[kExplosionRing];
        extern std::atomic<std::uint64_t> g_nextExplosion;
        extern thread_local bool t_playingExplosion;
        extern thread_local int t_playQueued;
        extern thread_local std::int32_t t_playQueueBefore;
        extern Slot<AnimAudioBatch> g_animBatches[kAnimRing];
        extern std::atomic<std::uint64_t> g_nextAnimBatch;
        extern thread_local int t_animScopeDepth;
        extern thread_local std::uint32_t t_animFootsteps;
        extern std::atomic<FootstepFn> g_originalFootstep;
        extern std::atomic<MovementUpdateFn> g_originalMovementUpdate;

        constexpr std::uint32_t kMeleeRing = 256;
        extern Slot<MeleeCall> g_melee[kMeleeRing];
        extern std::atomic<std::uint64_t> g_nextMelee;
        constexpr std::uint32_t kImpactRing = 256;
        extern Slot<ImpactCall> g_impacts[kImpactRing];
        extern std::atomic<std::uint64_t> g_nextImpact;
        extern Slot<SoundRequest> g_soundRequests[kSoundRequestRing];
        extern std::atomic<std::uint64_t> g_nextSoundRequest;
        extern std::atomic<SoundRequestFn> g_originalSoundRequest;
        extern Slot<SoundRequest> g_playRequests[kSoundRequestRing];
        extern std::atomic<std::uint64_t> g_nextPlayRequest;
        extern std::atomic<std::uint64_t> g_refusedPlayRequests;
        // Foot audios whose own foot-downs are not played (the replay's
        // puppets and the hidden player while a clip plays; the replay plays
        // the recorded steps through the original). Written by the script
        // thread, read by the detour on any thread; a stale entry for a moment
        // is harmless.
        constexpr std::size_t kMaxSilencedFeet = 256;
        extern std::atomic<std::uint64_t> g_silencedFeet[kMaxSilencedFeet];
        extern std::atomic<std::uint32_t> g_silencedFeetCount;
        extern std::atomic<std::uint64_t> g_silencedFootsteps;
        // puppet_audio_rule.h: the ped audio holders of the puppets, the
        // hidden player and his mount while a clip plays; a create-and-play on
        // them that the replay did not make is refused. Written by the script
        // thread, read by the detour on any thread.
        constexpr std::size_t kMaxSilencedPuppetAudios = 256;
        extern std::atomic<std::uint64_t> g_silencedPuppetAudio[kMaxSilencedPuppetAudios];
        extern std::atomic<std::uint32_t> g_silencedPuppetAudioCount;
        extern std::atomic<std::uint64_t> g_refusedPuppetOneShots;
        constexpr std::size_t kRefusedOneShotSamples = 16;
        extern std::atomic<std::uint64_t> g_refusedOneShotRef[kRefusedOneShotSamples];
        extern std::atomic<std::uint32_t> g_refusedOneShotCaller[kRefusedOneShotSamples];
        extern std::atomic<std::uint64_t> g_refusedOneShotBase;
        extern thread_local std::uint32_t t_replayEmitDepth;
        extern std::atomic<std::uint32_t> g_silencedPuppetOwner[kMaxSilencedPuppetAudios];
        bool SilencedPuppetAudioOwner(std::uint64_t audio, std::uint32_t& owner);
        // puppet_audio_rule.h: the weapon puppets' CWeapons (one-shots on
        // [CWeapon, CWeapon + kWeaponOneShotSpan) are refused like the
        // holders'), and the call sites of the clip's ESND ped audio sounds
        // (a puppet's own persistent create from one of them is refused).
        // Written by the script thread, read by the detours on any thread.
        constexpr std::size_t kMaxSilencedPuppetWeapons = 256;
        extern std::atomic<std::uint64_t> g_silencedPuppetWeapon[kMaxSilencedPuppetWeapons];
        extern std::atomic<std::uint32_t> g_silencedPuppetWeaponCount;
        extern std::atomic<std::uint64_t> g_refusedPuppetWeaponOneShots;
        constexpr std::size_t kMaxOwnPersistentCallers = 128;
        extern std::atomic<std::uint32_t> g_ownPersistentCaller[kMaxOwnPersistentCallers];
        extern std::atomic<std::uint32_t> g_ownPersistentCallerCount;
        extern std::atomic<bool> g_refuseOwnPersistent;
        extern std::atomic<std::uint64_t> g_refusedOwnPersistent;
        extern std::atomic<std::uint64_t> g_keptOwnPersistent;
        extern std::atomic<std::uint64_t> g_refusedOwnPersistentBase;
        extern std::atomic<std::uint64_t> g_keptOwnPersistentBase;
        constexpr std::size_t kOwnPersistentSamples = 12;
        extern std::atomic<std::uint64_t> g_refusedPersistentRef[kOwnPersistentSamples];
        extern std::atomic<std::uint32_t> g_refusedPersistentCaller[kOwnPersistentSamples];
        extern std::atomic<std::uint64_t> g_keptPersistentRef[kOwnPersistentSamples];
        extern std::atomic<std::uint32_t> g_keptPersistentCaller[kOwnPersistentSamples];
        extern std::atomic<bool> g_blockPuppetSounds;
        extern std::atomic<std::uint64_t> g_blockedPuppetCreates;
        constexpr std::size_t kBlockedRing = 8192;
        extern Slot<BlockedPuppetCreate> g_blocked[kBlockedRing];
        extern std::atomic<std::uint64_t> g_nextBlocked;
        extern std::atomic<const replay::soundowner::PacketLives*> g_blockLives;
        extern std::atomic<bool> g_blockAll;
        extern std::atomic<std::uint32_t> g_blockPlayheadMs;
        extern std::atomic<std::uint64_t> g_refusedUnmadeSets;
        extern std::atomic<std::uint64_t> g_unrecordedRefused;
        extern std::atomic<std::uint64_t> g_unrecordedKeptByEsnd;
        constexpr std::size_t kUnrecordedSamples = 8;
        extern std::atomic<std::uint32_t> g_unrecordedOwner[kUnrecordedSamples];
        extern std::atomic<std::uint64_t> g_unrecordedRef[kUnrecordedSamples];
        extern std::atomic<std::uint32_t> g_unrecordedSite[kUnrecordedSamples];
        extern std::atomic<std::uint32_t> g_unrecordedSampleCount;
        // puppet_audio_rule.h OwnPersistentCarried: the clip's ESND pedAudio
        // sounds as (entity, call site, entry) keys, sorted and published once
        // per playback.
        constexpr std::size_t kMaxOwnPersistentKeys = 4096;
        extern std::atomic<std::uint64_t> g_ownPersistentKey[kMaxOwnPersistentKeys];
        extern std::atomic<std::uint32_t> g_ownPersistentKeyCount;
        extern std::atomic<std::uint32_t> g_refusedPersistentOwner[kOwnPersistentSamples];
        extern std::atomic<std::uint32_t> g_refusedPersistentFrame[kOwnPersistentSamples][2];
        extern std::atomic<std::uint32_t> g_keptPersistentOwner[kOwnPersistentSamples];
        extern std::atomic<std::uint32_t> g_keptPersistentFrame[kOwnPersistentSamples][2];
        extern std::atomic<PersistentCreateFn> g_originalPersistentCreate;
        extern std::atomic<PersistentByRefFn> g_originalByRef;
        extern std::atomic<PersistentByHashFn> g_originalByHash;
        extern Slot<EntitySoundCreate> g_entitySounds[kEntitySoundRing];
        extern std::atomic<std::uint64_t> g_nextEntitySound;
        extern std::atomic<StopAndForgetFn> g_originalStopAndForget;
        constexpr std::uint32_t kStopRing2 = 256;
        extern Slot<SoundStop> g_soundStops[kStopRing2];
        extern std::atomic<std::uint64_t> g_nextSoundStop;
        extern std::atomic<MeleeImpactFn> g_originalMeleeImpact;

        constexpr std::uint32_t kFireRing = 256;
        extern Slot<FireCall> g_fires[kFireRing];
        extern std::atomic<std::uint64_t> g_nextFire;
        extern std::atomic<FireEnqueueFn> g_originalFire;
        extern std::uintptr_t g_fireDirect;
        extern std::atomic<SoundParamsCtorFn> g_originalCtor;
        extern std::atomic<bool> g_enabled;
        extern std::atomic<std::int32_t> g_inFlight;
        extern std::uintptr_t g_base;
        extern std::uintptr_t g_imageEnd;
        extern thread_local std::uint32_t t_replayedSystemDepth;
        struct ReplayedSystemScope
        {
            ReplayedSystemScope() { ++t_replayedSystemDepth; }
            ~ReplayedSystemScope() { --t_replayedSystemDepth; }
        };

        struct InFlight
        {
            InFlight() { g_inFlight.fetch_add(1, std::memory_order_seq_cst); }
            ~InFlight() { g_inFlight.fetch_sub(1, std::memory_order_seq_cst); }
        };
        std::uint64_t QpcUs();
        std::uint32_t ToRva(void* address);

        // The return addresses (call + 5) of the engine calls the rules
        // compare a caller with, as RVAs in this game (0 for a call the game
        // does not have: nothing matches it). Resolved once, by Install on
        // the script thread before any detour runs.
        struct CallerSites
        {
            replay::speech::CorePaths speechCore;
            replay::puppetaudio::SpeechControllerCallers speechController;
            footevent::CallReturns footEvent;
            footevent::StepCallers footSteps;
            replay::mountaudio::SiteReturns mount;
            std::uint32_t playerAudio[PlayerAudioStats::kSites - 1] = {};
            std::uint32_t collisionCreate = 0;
        };
        const CallerSites& Callers();
        bool GuardedRead(void* out, const void* source, std::size_t size);

        struct FootFields
        {
            bool slot = false;
            bool layers = false;
            bool wetness = false;
            bool mix = false;
            bool impact = false;
        };
        const FootFields& FootProof();
        void __fastcall FootstepDetour(void* footAudio, std::int32_t eventId, std::uint32_t arg3,
            std::uint8_t foot, std::uint8_t arg5, std::uint8_t flag, std::uint32_t arg7, std::uint8_t arg8);
        void __fastcall MovementUpdateDetour(void* footAudio);
        extern std::atomic<FootEventFn> g_originalFootEvent;
        extern void* g_footEventTarget;
        extern const char* g_footEventError;
        extern std::atomic<bool> g_footEventRefuseOwn;
        void __fastcall FootEventDetour(void* footAudio, std::int32_t type, std::uint8_t foot, std::uint8_t spread,
            std::uint8_t direct);

        // Audio categories (init params +0xD8) seen in live requests of this
        // process. Recorded params keep the category pointer the game gave,
        // but a pointer from a clip is only trusted when this process has seen
        // the same category object: categories live as long as the audio
        // engine, so such a pointer is valid here.
        constexpr std::size_t kSoundCategoryOffset = 0xD8;
        bool KnownSoundCategory(std::uint64_t category);
        void NoteBlocked(std::uint8_t port, std::uint32_t owner, std::uint64_t soundRef, std::uint32_t caller,
            replay::soundowner::LifeVerdict verdict, bool refused);
        void ExpectQueuedCreate(std::uint64_t audio, std::uint64_t soundRef);

        // Mode c's tickets. Each request through the queue holds one entry
        // (audio, ref, its window); the creator (census, S01/S02) gives the
        // first sound it makes for that (audio, ref) to the oldest waiting
        // entry and writes the birth to a ring the player drains by ticket.
        // Script thread writes the entries, any thread takes them.
        constexpr std::size_t kQueueTickets = 64;
        extern std::atomic<std::uint64_t> g_ticketAudio[kQueueTickets];
        extern std::atomic<std::uint64_t> g_ticketRef[kQueueTickets];
        extern std::atomic<std::uint64_t> g_ticketUntilUs[kQueueTickets];
        extern std::atomic<std::uint64_t> g_ticketId[kQueueTickets];
        extern std::atomic<std::uint32_t> g_ticketsWaiting;
        using QueueBirthSlot = replay::ring::Slot<QueueBirth>;
        constexpr std::size_t kQueueBirthRing = 256;
        extern QueueBirthSlot g_queueBirths[kQueueBirthRing];
        extern std::atomic<std::uint64_t> g_queueBirthNext;
        std::uint64_t OpenQueueTicket(std::uint64_t audio, std::uint64_t soundRef);
        void* PrepareAndPlayPort();

        template <typename Fn>
        bool Hook(const sig::Signature& signature, void* detour, std::atomic<Fn>& original, void*& target,
            const char* name, std::string& error)
        {
            void* address = reinterpret_cast<void*>(sig::Find(signature));
            if (!address)
            {
                error += std::string(name) + "_signature_mismatch;";
                return false;
            }
            void* trampoline = nullptr;
            if (MH_CreateHook(address, detour, &trampoline) != MH_OK || !trampoline)
            {
                error += std::string(name) + "_create_failed;";
                return false;
            }
            original.store(reinterpret_cast<Fn>(trampoline), std::memory_order_release);
            if (replay::hooks::Enable(address) != MH_OK)
            {
                MH_RemoveHook(address);
                original.store(nullptr, std::memory_order_release);
                error += std::string(name) + "_enable_failed;";
                return false;
            }
            target = address;
            return true;
        }

        // A window of what is published now, by time.
        template <typename T>
        void Collect(const Slot<T>* ring, std::uint32_t size, const std::atomic<std::uint64_t>& next,
            std::uint64_t fromUs, std::uint64_t toUs, std::vector<T>& out)
        {
            out.clear();
            replay::ring::ForEachPublished(ring, size, next, [&](const T& value) {
                if (value.timeUs >= fromUs && value.timeUs <= toUs)
                    out.push_back(value);
            });
        }

        // Everything since the cursor, in order; the cursor stops at a
        // ticket not yet published and the next call reads it.
        template <typename T>
        void CollectSince(const Slot<T>* ring, std::uint32_t size, const std::atomic<std::uint64_t>& next,
            std::uint64_t& cursor, std::vector<T>& out)
        {
            out.clear();
            replay::ring::ForEachSince(ring, size, next, cursor, [&](const T& value) { out.push_back(value); });
        }
        bool GuardedPedAudio(void* ped, std::uint8_t*& audio);
        bool IsFootAudio(std::uint64_t vtable);
        extern const sig::Signature kFirePlay;
        extern const sig::Signature kRefContainer;
        bool GuardedRefContainer(std::uintptr_t fn, std::uint32_t key, void*& out);
        void CopyRecordedSoundParams(std::uint8_t* params, const std::uint8_t* recorded);
        std::uint64_t RequestedSettings(std::uint64_t sound);
        extern std::atomic<std::uint64_t> g_linkChecks;
        extern std::atomic<std::uint64_t> g_linkNotSound;
        extern std::atomic<std::uint64_t> g_linkNoSettings;
        extern std::atomic<std::uint64_t> g_linkBroken;
        extern std::atomic<std::uint64_t> g_linkRefused[kSoundLinkPorts];
        extern std::atomic<std::uint64_t> g_persistentLinkBroken;
        std::uint64_t LiveSettings(std::uint64_t sound);
        std::uint64_t PortSettings(std::uint64_t sound, SoundLinkPort port);
        bool GuardedCall(void (*body)(void*), void* context, std::uint32_t& code);

    // Player weapon sound on the main character's puppet (see the header).
    // The predicate, whole function (0x4E bytes, ends in ret):
    //   cmp byte [rcx+30h],4 / jne no ; ped only
    //   row = [table] + ((dword [rcx+9Ch] & 1FFFFh) - [base index]) * 148h
    //   comp = [row+0B8h] & ~1 ; flags = byte [comp+40h]
    //   yes when !(flags & 1) && (flags & 2)
        constexpr std::size_t kPlayerAudioComponentSlot = 0xB8;
        constexpr std::size_t kPlayerAudioFlags = 0x40;
        using PlayerAudioPredicateFn = bool(__fastcall*)(const void*);
        extern const sig::Signature* const kPlayerAudioSites[PlayerAudioStats::kSites - 1];
        extern std::atomic<PlayerAudioPredicateFn> g_originalPlayerAudio;
        extern void* g_playerAudioTarget;
        constexpr std::size_t kMountLinkSlot = 0xA8; // component row slot the predicate is given
        using MountAudioPredicateFn = bool(__fastcall*)(const void*);
        constexpr std::size_t kMountAudioComponents = 4;
        extern std::atomic<std::uint64_t> g_mountAudioComponents[kMountAudioComponents];
        extern std::atomic<MountAudioPredicateFn> g_originalMountAudio;
        extern void* g_mountAudioTarget;
        extern void* g_movementVolumeTarget;
        extern std::atomic<bool> g_movementDetailOn;
}
