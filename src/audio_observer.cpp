// audio_observer.cpp: the detours and their rings (sound params, speech, explosions, anim, sound requests, creates, persistent sounds, melee), Install, the switches, Shutdown.
// The other families are in audio_observer_*.cpp; shared internals in audio_observer_internal.h.

#include "audio_observer_internal.h"

namespace replay::audio
{

        // The game code this module calls and detours, found by signature
        // (game_sig_core.h): Find gives 0 for one the game does not have, and
        // the feature that needs it stays off.
        const sig::Signature kSoundParamsCtor{"audio_sound_params_ctor",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 20 0F 57 C0 48 8D 41 10",
            0x487E34};
        const sig::Signature kSpeechSay{"audio_speech_say", speech::kAnimalVocal, sig::Rel32{0x1E, 0x22},
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 8B DA 48 8D 4C 24 20 E8",
            0x2F9FC84};
    namespace
    {

        // Pure getter: ped -> ped audio entity (reads the model table only).
        const sig::Signature kPedAudioGetter{"audio_ped_audio_getter",
            "8B 81 9C 00 00 00 25 FF FF 01 00 2B 05 ?? ?? ?? ?? 48 69 C8 48 01 00 00 48 8B 05 ?? ?? ?? ?? 48 8B 94 01 "
            "B0 00 00 00", 0xCC81FC};
    }

        const sig::Signature kFireEnqueue{"audio_fire_enqueue",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 83 F8 14 74 1D 48 8D 0C",
            0x2EE8D9C};

        // Deferred create-and-play: 8 arguments (audio entity, u64 sound ref,
        // owner entity, init params, then four byte flags on the stack at
        // entry +0x28..+0x40); returns bool.
        const sig::Signature kSoundRequest{"audio_sound_request",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 55 41 56 41 57 48 8D 68 C1 48 81 EC A0 00 00 00 83 C8 FF 45 "
            "33 FF 44 89 7D D7 48 8B DA 66 83 4D D9 FC 48 8D 55 C7 66 89 45 D7 4C 8B F1 8A 45 EF 48 8D 4D F7 24 FC 4C "
            "89 7D DF 0A 45 67 49 8B F9 0C 3C 44 89 7D E7 88 45 EF 49 8B F0 E8 ?? ?? ?? ?? 4C 8B C8 4C 8B C6 8A 45 7F "
            "48 8B D3 88 44 24 38 49 8B CE 8A 45 77 88 44 24 30 8A 45 6F",
            0x310E680};
        // Release of the init params handle at +0x12C (no-op when -1).
        const sig::Signature kSoundParamsRelease{"audio_sound_params_release",
            "83 CA FF 66 39 11 74 12 0F BF 09 48 8B 05 ?? ?? ?? ??", 0x253F0D0};
    namespace
    {
        // audEntity::CreateAndPlaySound core: audio entity, u64 sound ref,
        // init params; queues a one-shot. Reached through the public entry
        // (ref) and the one by hash.
        const sig::Signature kCreateAndPlay{"audio_create_and_play",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 10 57 48 81 EC 60 01 00 00 8B 1D ?? ?? ?? ??",
            0x2589A40};
        using CreateAndPlayFn = bool(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t);
    }
        // Persistent sound creation used by collision code (audio entity,
        // sound descriptor with the ref at +0xC, audSound** out, init params
        // or null); the caller plays and updates the sound. Its twin differs
        // only in its call targets, so it is reached from the collision code,
        // which first loads the static collision audio entity.
        const sig::Signature kStaticCollisionAudio{"audio_static_collision_audio", kAudioCollisionSite,
            sig::Rel32{0x3, 0x7}, "", 0x5C10540};
        // The persistent create cores (audio entity, ref or hash, audSound**
        // out, init params or null); both start with `test r8, r8; je` (the
        // out slot).
        const sig::Signature kPersistentByRef{"audio_persistent_by_ref",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 89 6C 24 10 48 89 74 24 20 57 41 56 41 57 48 81 EC 90 01 00 "
            "00 83 79 14 00 49 8B E9 4D 8B F0 48 8B DA",
            0x2589124};
        const sig::Signature kPersistentByHash{"audio_persistent_by_hash",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 81 EC 70 01 "
            "00 00 83 79 14 00 49 8B E9 4D 8B F0 48 8B DA",
            0x2589058};
    namespace
    {
        constexpr std::uint32_t kEntityTracked = 256;
    }
        // StopAndForget (sound, continueUpdatingEntity); its body reads the
        // requested-settings pool (globals at +0x1D and +0x2B, rip-relative).
        const sig::Signature kStopAndForget{"audio_stop_and_forget",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? F9 FF FF 00 00 74 27 41 0F B6 81 A0 00 00 00 0F AF 0D ?? ?? ?? "
            "?? 4C 69 C0 60 A8 02 00 48 8B 05 ?? ?? ?? ??",
            0x2585C2C};
    namespace
    {
        const sig::Signature kPrepareAndPlay{"audio_prepare_and_play",
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 33 DB 41 8B F9 45 8A F0 48 8B "
            "F2 48 8B E9 48 85 D2 74 10 48 8B CA E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 8A 85 EE 00 00 00 A8 02 0F 85 "
            "?? ?? ?? ?? 41 83 CA FF 48 85 F6 74 1B 48 2B 35 ?? ?? ?? ?? 33 D2 48 63 0D ?? ?? ?? ?? 48 8B C6 48 F7 F1 "
            "4C 8B C0 EB 04 45 0F B7 C2 41 3B FA 75 07 B9 30 75 00 00 EB 14 85 FF 79 04 8B CB EB 0C B9 30 75 00 00 3B "
            "F9 0F 4F F9 8B CF 44 8B 0D ?? ?? ?? ?? 0F B7 95 D6 00 00 00 44 03 C9 81 FA FF FF 00 00 74 26 0F B6 85 A0 "
            "00 00 00 8B 1D ?? ?? ?? ?? 48 69 C8 60 A8 02 00 0F AF DA 48 8B 05 ?? ?? ?? ?? 48 03 9C 01 50 A8 02 00 80 "
            "A3 60 02 00 00 DF B9 FF 00 00 00 41 C0 E6 05 44 08 B3 60 02 00 00 66 45 3B C2 41 0F B6 C0 0F 44 C1 88 83 "
            "5E 02 00 00 8A 44 24 50",
            0x257CCBC};
    }
        // Public CreateAndPlaySound (ref): registers the audio entity when
        // needed, then calls the core.
        const sig::Signature kCreateAndPlayPublic{"audio_create_and_play_public",
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 83 79 14 00 49 8B F0 48 8B DA",
            0x2588950};
    namespace
    {

        using MeleeBodyImpactFn = void(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t);
        using SpeechSayFn = std::uint64_t(__fastcall*)(void*, void*);

        // Speech core: every line of every path is validated and created here
        // (speech entity, scope, internal params).
        const sig::Signature kSpeechCore{"audio_speech_core",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 20 57 48 81 EC 10 02 00 00",
            0x2FA1114};
        using SpeechCoreFn = std::uint32_t(__fastcall*)(void*, void*, void*);
    }
        const sig::Signature kSpeechScopeRelease{"audio_speech_scope_release",
            "40 53 48 83 EC 20 48 8B D9 48 8B 49 28 48 85 C9 74 0D", 0x2FBC728};

        // Line stop (line + reason): every path that ends a line before or
        // after it plays goes through here (50 call sites).
        const sig::Signature kSpeechLineStop{"audio_speech_line_stop",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 57 48 83 EC 20 83 B9 34 02 00 00 06 8B F2",
            0x2FBCCD4};
    namespace
    {

        // Audible start of a line (called twice by the line update; plays the
        // line data (vtable +0x58) and moves the line to state 5).
        const sig::Signature kSpeechLineAudible{"audio_speech_line_audible",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 48 89 78 20 41 56 48 83 EC 30 48 8B F9 45 33 F6",
            0x2FB99E8};
        using SpeechLineAudibleFn = void(__fastcall*)(void*);
    }

        // Footstep entry: (footAudio, id, arg3, foot, arg5, flag, arg7, arg8);
        // the body reads flag and arg7 from the stack. Called by the foot-down
        // handler with holder +0x1560 once holder +0x1584 is set.
        const sig::Signature kFootstep{"audio_footstep",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 40 45 8B E8 45 "
            "8A F9 45 33 C0 8B FA 48 8B D9",
            0x317A340};
    namespace
    {

        // Explosion audio.
        const sig::Signature kExplosionEnqueue{"audio_explosion_enqueue",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 63 89 00 04 00 00 83",
            0x2EF0010};
        using ExplosionEnqueueFn = void(__fastcall*)(void* manager, const void* request);
        const sig::Signature kPedAnimHandler{"audio_ped_anim_handler",
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 20 48 8D 59 78 48 8B F9 48 63 43 60",
            0x3193CAC};
        using PedAnimHandlerFn = void(__fastcall*)(void* audio);
        constexpr std::uint32_t kSoundRing = 16384;
        constexpr std::uint32_t kSpeechRing = 1024;

        Slot<SoundCall> g_sounds[kSoundRing];
        std::atomic<std::uint64_t> g_nextSound{0};
        Slot<SpeechCall> g_speech[kSpeechRing];
        std::atomic<std::uint64_t> g_nextSpeech{0};
    }
        Slot<SpeechCoreCall> g_core[kCoreRing];
        std::atomic<std::uint64_t> g_nextCore{0};
    namespace
    {
        std::atomic<SpeechCoreFn> g_originalCore{nullptr};
        void* g_coreTarget = nullptr;
    }
        Slot<SpeechLineStop> g_stops[kStopRing];
        std::atomic<std::uint64_t> g_nextStop{0};
        std::atomic<std::uint64_t> g_puppetSpeech[kMaxPuppetSpeech] = {};
        std::atomic<std::uint32_t> g_puppetSpeechOwner[kMaxPuppetSpeech] = {};
        std::atomic<std::uint32_t> g_puppetSpeechCount{0};
        std::atomic<bool> g_puppetOwnSpeechBlock{true};
        std::atomic<std::uintptr_t> g_ownSpeechRelease{0};
        const char* g_ownSpeechError = "not_installed";
        std::atomic<std::uint64_t> g_puppetOwnSpeechSeen{0};
        std::atomic<std::uint64_t> g_puppetOwnSpeechRefused{0};
        // Core calls on other speech entities while puppets are listed (the
        // live world talking behind the replay: its lines take wave slots).
        std::atomic<std::uint64_t> g_worldSpeechDuringPlayback{0};
        std::atomic<std::uint64_t> g_worldSpeechQueuedDuringPlayback{0};
        Slot<PuppetOwnSpeech> g_ownSpeech[kOwnSpeechRing];
        std::atomic<std::uint64_t> g_nextOwnSpeech{0};

        bool PuppetSpeechOwner(std::uint64_t speech, std::uint32_t& owner)
        {
            const std::uint32_t count = g_puppetSpeechCount.load(std::memory_order_acquire);
            if (!count || !speech)
                return false;
            const std::uint32_t used = count < kMaxPuppetSpeech ? count : static_cast<std::uint32_t>(kMaxPuppetSpeech);
            for (std::uint32_t n = 0; n < used; ++n)
                if (g_puppetSpeech[n].load(std::memory_order_relaxed) == speech)
                {
                    owner = g_puppetSpeechOwner[n].load(std::memory_order_relaxed);
                    return true;
                }
            return false;
        }
        std::atomic<SpeechLineStopFn> g_originalStop{nullptr};
    namespace
    {
        void* g_stopTarget = nullptr;
    }
        Slot<SpeechLineStart> g_starts[kStartRing];
        std::atomic<std::uint64_t> g_nextStart{0};
    namespace
    {
        std::atomic<SpeechLineAudibleFn> g_originalAudible{nullptr};
    }
        void* g_audibleTarget = nullptr;
        Slot<ExplosionAudioCall> g_explosions[kExplosionRing];
        std::atomic<std::uint64_t> g_nextExplosion{0};
    namespace
    {
        std::atomic<ExplosionEnqueueFn> g_originalExplosion{nullptr};
        void* g_explosionTarget = nullptr;
    }
        thread_local bool t_playingExplosion = false;
        // What the enqueuer did with our playback request (-1 not reached,
        // 0 not stored, 1 stored), read by the detour.
        thread_local int t_playQueued = -1;
        thread_local std::int32_t t_playQueueBefore = -1;
        Slot<AnimAudioBatch> g_animBatches[kAnimRing];
        std::atomic<std::uint64_t> g_nextAnimBatch{0};
    namespace
    {
        std::atomic<PedAnimHandlerFn> g_originalPedAnim{nullptr};
        void* g_pedAnimTarget = nullptr;
    }
        // Counters of what the engine creates inside one ped anim handler call.
        thread_local int t_animScopeDepth = 0;
    namespace
    {
        thread_local std::uint32_t t_animSounds = 0;
    }
        thread_local std::uint32_t t_animFootsteps = 0;
    namespace
    {
        thread_local std::uint32_t t_animSpeech = 0;
    }
        std::atomic<FootstepFn> g_originalFootstep{nullptr};
    namespace
    {
        void* g_footstepTarget = nullptr;
    }
        std::atomic<MovementUpdateFn> g_originalMovementUpdate{nullptr};
    namespace
    {
        void* g_movementUpdateTarget = nullptr;
    }
        Slot<MeleeCall> g_melee[kMeleeRing];
        std::atomic<std::uint64_t> g_nextMelee{0};
        Slot<ImpactCall> g_impacts[kImpactRing];
        std::atomic<std::uint64_t> g_nextImpact{0};
    namespace
    {
        std::atomic<MeleeBodyImpactFn> g_originalBodyImpact{nullptr};
        void* g_bodyImpactTarget = nullptr;
    }
        Slot<SoundRequest> g_soundRequests[kSoundRequestRing];
        std::atomic<std::uint64_t> g_nextSoundRequest{0};
        std::atomic<SoundRequestFn> g_originalSoundRequest{nullptr};
    namespace
    {
        void* g_soundRequestTarget = nullptr;
    }
        Slot<SoundRequest> g_playRequests[kSoundRequestRing];
        std::atomic<std::uint64_t> g_nextPlayRequest{0};
        std::atomic<std::uint64_t> g_refusedPlayRequests{0}; // create-and-play calls the engine refused (not recorded)
        std::atomic<std::uint64_t> g_silencedFeet[kMaxSilencedFeet] = {};
        std::atomic<std::uint32_t> g_silencedFeetCount{0};
        std::atomic<std::uint64_t> g_silencedFootsteps{0};
        std::atomic<std::uint64_t> g_silencedPuppetAudio[kMaxSilencedPuppetAudios] = {};
        std::atomic<std::uint32_t> g_silencedPuppetAudioCount{0};
        std::atomic<std::uint64_t> g_refusedPuppetOneShots{0};
        std::atomic<std::uint64_t> g_refusedOneShotRef[kRefusedOneShotSamples] = {};
        std::atomic<std::uint32_t> g_refusedOneShotCaller[kRefusedOneShotSamples] = {};
        std::atomic<std::uint64_t> g_refusedOneShotBase{0}; // samples count from here (BeginRefusedPuppetOneShots)
        thread_local std::uint32_t t_replayEmitDepth = 0;
    namespace
    {

        bool IsSilencedPuppetAudio(std::uint64_t audio)
        {
            const std::uint32_t count = g_silencedPuppetAudioCount.load(std::memory_order_acquire);
            if (!count)
                return false;
            std::uint64_t holders[kMaxSilencedPuppetAudios];
            const std::uint32_t used = count < kMaxSilencedPuppetAudios ? count : static_cast<std::uint32_t>(kMaxSilencedPuppetAudios);
            for (std::uint32_t n = 0; n < used; ++n)
                holders[n] = g_silencedPuppetAudio[n].load(std::memory_order_relaxed);
            return replay::puppetaudio::InSpans(audio, holders, used, kPedAudioSpan);
        }
    }
        // puppet_audio_rule.h OwnPersistentCarried: the clip entity of each
        // silenced holder (same index), kNoOwner for the hidden player and his
        // mount. Written before the holder.
        std::atomic<std::uint32_t> g_silencedPuppetOwner[kMaxSilencedPuppetAudios] = {};

        // The silenced holder the audio entity lies in, with its clip entity.
        bool SilencedPuppetAudioOwner(std::uint64_t audio, std::uint32_t& owner)
        {
            owner = replay::puppetaudio::kNoOwner;
            const std::uint32_t count = g_silencedPuppetAudioCount.load(std::memory_order_acquire);
            if (!count || !audio)
                return false;
            const std::uint32_t used = count < kMaxSilencedPuppetAudios ? count : static_cast<std::uint32_t>(kMaxSilencedPuppetAudios);
            for (std::uint32_t n = 0; n < used; ++n)
            {
                const std::uint64_t holder = g_silencedPuppetAudio[n].load(std::memory_order_relaxed);
                if (!holder || audio < holder || audio - holder >= kPedAudioSpan)
                    continue;
                owner = g_silencedPuppetOwner[n].load(std::memory_order_relaxed);
                return true;
            }
            return false;
        }
        std::atomic<std::uint64_t> g_silencedPuppetWeapon[kMaxSilencedPuppetWeapons] = {};
        std::atomic<std::uint32_t> g_silencedPuppetWeaponCount{0};
        std::atomic<std::uint64_t> g_refusedPuppetWeaponOneShots{0};
        std::atomic<std::uint32_t> g_ownPersistentCaller[kMaxOwnPersistentCallers] = {};
        std::atomic<std::uint32_t> g_ownPersistentCallerCount{0};
        std::atomic<bool> g_refuseOwnPersistent{true};
        std::atomic<std::uint64_t> g_refusedOwnPersistent{0};
        std::atomic<std::uint64_t> g_keptOwnPersistent{0};
        std::atomic<std::uint64_t> g_refusedOwnPersistentBase{0};
        std::atomic<std::uint64_t> g_keptOwnPersistentBase{0};
        std::atomic<std::uint64_t> g_refusedPersistentRef[kOwnPersistentSamples] = {};
        std::atomic<std::uint32_t> g_refusedPersistentCaller[kOwnPersistentSamples] = {};
        std::atomic<std::uint64_t> g_keptPersistentRef[kOwnPersistentSamples] = {};
        std::atomic<std::uint32_t> g_keptPersistentCaller[kOwnPersistentSamples] = {};

        // SoundGenericMode=packets: while on, every create of the engine's own
        // on a silenced holder (the puppets, the hidden player and his mount)
        // is refused at the ports S03, S04, S05 and S06 unless the replay makes
        // it (ReplayEmitScope, a replayed footstep or melee call) or the ped's
        // sound controller of the speech makes it (puppet_audio_rule.h
        // BlockPuppetCreate). Each refusal goes to a ring the script thread
        // drains for the sound check.
        std::atomic<bool> g_blockPuppetSounds{false};
        std::atomic<std::uint64_t> g_blockedPuppetCreates{0};
        Slot<BlockedPuppetCreate> g_blocked[kBlockedRing];
        std::atomic<std::uint64_t> g_nextBlocked{0};
        // What the ports judge against (sound_owner_table.h PacketLives,
        // published by the generic player at its start) and the playhead;
        // SoundGenericBlock=all refuses everything.
        std::atomic<const replay::soundowner::PacketLives*> g_blockLives{nullptr};
        std::atomic<bool> g_blockAll{false};
        std::atomic<std::uint32_t> g_blockPlayheadMs{0};
        // puppet_audio_rule.h RefuseUnmadeReplayedSet.
        std::atomic<std::uint64_t> g_refusedUnmadeSets{0};
        // puppet_audio_rule.h RefuseUnrecordedOwnPersistent: the count and the
        // first refusals (owner, ref, call site) for the evidence line.
        std::atomic<std::uint64_t> g_unrecordedRefused{0};
        std::atomic<std::uint64_t> g_unrecordedKeptByEsnd{0};
        std::atomic<std::uint32_t> g_unrecordedOwner[kUnrecordedSamples] = {};
        std::atomic<std::uint64_t> g_unrecordedRef[kUnrecordedSamples] = {};
        std::atomic<std::uint32_t> g_unrecordedSite[kUnrecordedSamples] = {};
        std::atomic<std::uint32_t> g_unrecordedSampleCount{0};
    namespace
    {

        bool IsSilencedPuppetWeaponAudio(std::uint64_t audio)
        {
            const std::uint32_t count = g_silencedPuppetWeaponCount.load(std::memory_order_acquire);
            if (!count)
                return false;
            std::uint64_t weapons[kMaxSilencedPuppetWeapons];
            const std::uint32_t used = count < kMaxSilencedPuppetWeapons ? count : static_cast<std::uint32_t>(kMaxSilencedPuppetWeapons);
            for (std::uint32_t n = 0; n < used; ++n)
                weapons[n] = g_silencedPuppetWeapon[n].load(std::memory_order_relaxed);
            return replay::puppetaudio::InSpans(audio, weapons, used, replay::puppetaudio::kWeaponOneShotSpan);
        }

        bool OwnPersistentCallerCarried(std::uint32_t caller)
        {
            const std::uint32_t count = g_ownPersistentCallerCount.load(std::memory_order_acquire);
            std::uint32_t callers[kMaxOwnPersistentCallers];
            const std::uint32_t used = count < kMaxOwnPersistentCallers ? count : static_cast<std::uint32_t>(kMaxOwnPersistentCallers);
            for (std::uint32_t n = 0; n < used; ++n)
                callers[n] = g_ownPersistentCaller[n].load(std::memory_order_relaxed);
            return replay::puppetaudio::CallerCarried(caller, callers, used);
        }
    }
        std::atomic<std::uint64_t> g_ownPersistentKey[kMaxOwnPersistentKeys] = {};
        std::atomic<std::uint32_t> g_ownPersistentKeyCount{0};
        // The samples' holder entity and the two game frames above the call
        // site (evidence: which puppet, which engine path).
        std::atomic<std::uint32_t> g_refusedPersistentOwner[kOwnPersistentSamples] = {};
        std::atomic<std::uint32_t> g_refusedPersistentFrame[kOwnPersistentSamples][2] = {};
        std::atomic<std::uint32_t> g_keptPersistentOwner[kOwnPersistentSamples] = {};
        std::atomic<std::uint32_t> g_keptPersistentFrame[kOwnPersistentSamples][2] = {};
    namespace
    {

        // puppetaudio::OwnPersistentCarried on the published lists, read in
        // place (SortedKeyCarried's binary search on the atomics, no copy of
        // the keys on the detour's stack).
        bool OwnPersistentCreateCarried(std::uint32_t owner, std::uint32_t caller, std::uint64_t soundRef, bool byHash)
        {
            if (!caller)
                return false;
            if (owner == replay::puppetaudio::kNoOwner)
                return OwnPersistentCallerCarried(caller);
            const std::uint64_t key =
                replay::puppetaudio::OwnPersistentKey(owner, caller, replay::puppetaudio::SoundEntryKey(soundRef, byHash));
            const std::uint32_t count = g_ownPersistentKeyCount.load(std::memory_order_acquire);
            std::uint32_t low = 0;
            std::uint32_t high = count < kMaxOwnPersistentKeys ? count : static_cast<std::uint32_t>(kMaxOwnPersistentKeys);
            while (low < high)
            {
                const std::uint32_t middle = low + (high - low) / 2;
                const std::uint64_t value = g_ownPersistentKey[middle].load(std::memory_order_relaxed);
                if (value == key)
                    return true;
                if (value < key)
                    low = middle + 1;
                else
                    high = middle;
            }
            return false;
        }
        std::atomic<CreateAndPlayFn> g_originalCreateAndPlay{nullptr};
        void* g_createAndPlayTarget = nullptr;
    }
        std::atomic<PersistentCreateFn> g_originalPersistentCreate{nullptr};
    namespace
    {
        void* g_persistentCreateTarget = nullptr;
    }
        std::atomic<PersistentByRefFn> g_originalByRef{nullptr};
    namespace
    {
        void* g_byRefTarget = nullptr;
    }
        std::atomic<PersistentByHashFn> g_originalByHash{nullptr};
    namespace
    {
        void* g_byHashTarget = nullptr;
    }
        Slot<EntitySoundCreate> g_entitySounds[kEntitySoundRing];
        std::atomic<std::uint64_t> g_nextEntitySound{0};
    namespace
    {
        // Entity sounds created while recording: their stop is recorded too.
        std::atomic<std::uint64_t> g_entityTracked[kEntityTracked] = {};
        std::atomic<std::uint32_t> g_nextEntityTracked{0};
    }
        std::atomic<StopAndForgetFn> g_originalStopAndForget{nullptr};
    namespace
    {
        void* g_stopAndForgetTarget = nullptr;
        // Collision sounds created while recording, so the stop detour
        // records only those (547 callers stop every kind of sound).
        constexpr std::uint32_t kTrackedSounds = 64;
        std::atomic<std::uint64_t> g_trackedSounds[kTrackedSounds] = {};
        std::atomic<std::uint32_t> g_nextTracked{0};
    }
        Slot<SoundStop> g_soundStops[kStopRing2];
        std::atomic<std::uint64_t> g_nextSoundStop{0};
        std::atomic<MeleeImpactFn> g_originalMeleeImpact{nullptr};
    namespace
    {
        void* g_meleeImpactTarget = nullptr;
    }
        Slot<FireCall> g_fires[kFireRing];
        std::atomic<std::uint64_t> g_nextFire{0};
        std::atomic<FireEnqueueFn> g_originalFire{nullptr};
    namespace
    {
        void* g_fireTarget = nullptr;
    }
        std::uintptr_t g_fireDirect = 0;

        std::atomic<SoundParamsCtorFn> g_originalCtor{nullptr};
    namespace
    {
        std::atomic<SpeechSayFn> g_originalSay{nullptr};
    }
        std::atomic<bool> g_enabled{false};
    namespace
    {
        constexpr std::uint32_t kShortCallerDepth = 3; // callers[0..2], what POSE_V2_AUDIO_TOP reads
        std::atomic<std::uint32_t> g_soundCallerDepth{kShortCallerDepth};
        // horse_audio_evidence.h: the create-and-play ring also records while
        // a clip plays (evidence only; nothing reads it but the horse audio
        // evidence, which has its own cursor).
        std::atomic<bool> g_playRequestEvidence{false};
        std::atomic<bool> g_playRequestMixProbe{false}; // the mix probe reads the ring
        // Playback watch of the entity creates (the puppets' own).
        std::atomic<bool> g_observeEntityCreates{false};
    }
        std::atomic<std::int32_t> g_inFlight{0};
    namespace
    {
        void* g_ctorTarget = nullptr;
        void* g_sayTarget = nullptr;
    }
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
    namespace
    {
        LARGE_INTEGER g_frequency{};
    }

        // Depth of footstep/melee action calls on this thread: sounds made
        // inside them are replayed by those systems, not as generic sounds.
        thread_local std::uint32_t t_replayedSystemDepth = 0;

        std::uint64_t QpcUs()
        {
            return replay::clock::NowUs(); // mod_clock.h (no 21-day overflow)
        }

        std::uint32_t ToRva(void* address)
        {
            const auto value = reinterpret_cast<std::uintptr_t>(address);
            return value >= g_base && value < g_imageEnd ? static_cast<std::uint32_t>(value - g_base) : 0;
        }

        bool GuardedRead(void* out, const void* source, std::size_t size)
        {
            // Garbage pointers (below 4 GB, where the game's heaps never are,
            // or non-canonical) are refused without a fault. A fault is caught
            // here, but its dispatch runs every vectored handler in the process
            // first; another mod's CLR handler can overflow the script fiber's
            // stack on one.
            return replay::mem::Copy(out, source, size, replay::mem::kGameHeapLowest);
        }
    namespace
    {

        void* __fastcall SoundParamsCtorDetour(void* params)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.audio_params");
            InFlight inFlight;
            const SoundParamsCtorFn original = g_originalCtor.load(std::memory_order_acquire);
            void* result = original ? original(params) : params;
            if (replay::soundfx::Active()) // params built per second against sounds created
                replay::soundfx::NoteCall(replay::soundfx::kParams, _ReturnAddress());
            if (t_animScopeDepth)
                ++t_animSounds;
            if (!g_enabled.load(std::memory_order_relaxed))
                return result;
            void* frames[kCallerDepth] = {};
            const std::uint32_t depth = g_soundCallerDepth.load(std::memory_order_relaxed);
            const USHORT captured = depth ? RtlCaptureStackBackTrace(1, static_cast<ULONG>(depth), frames, nullptr) : 0;
            replay::ring::Writer<SoundCall> slot(g_sounds, kSoundRing, g_nextSound);
            slot.value.timeUs = QpcUs();
            for (std::uint32_t frame = 0; frame < kCallerDepth; ++frame)
                slot.value.callers[frame] = frame < captured ? ToRva(frames[frame]) : 0;
            slot.Publish();
            return result;
        }

        std::uint64_t __fastcall SpeechSayDetour(void* speechEntity, void* params)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.audio_speech_say");
            InFlight inFlight;
            SpeechCall call;
            if (g_enabled.load(std::memory_order_relaxed) && params)
            {
                const auto* bytes = static_cast<const std::uint8_t*>(params);
                GuardedRead(&call.voiceHash, bytes + kParamsVoice, 4);
                GuardedRead(&call.contextPartialHash, bytes + kParamsContext, 4);
                GuardedRead(&call.variation, bytes + kParamsVariation, 4);
            }
            const SpeechSayFn original = g_originalSay.load(std::memory_order_acquire);
            const std::uint64_t result = original ? original(speechEntity, params) : 0;
            if (!g_enabled.load(std::memory_order_relaxed))
                return result;
            void* frames[4] = {};
            const USHORT captured = RtlCaptureStackBackTrace(1, 4, frames, nullptr);
            call.timeUs = QpcUs();
            call.speechEntity = reinterpret_cast<std::uint64_t>(speechEntity);
            call.result = result;
            for (std::uint32_t depth = 0; depth < 4; ++depth)
                call.callers[depth] = depth < captured ? ToRva(frames[depth]) : 0;
            replay::ring::Writer<SpeechCall> slot(g_speech, kSpeechRing, g_nextSpeech);
            slot.value = call;
            slot.Publish();
            return result;
        }

        // Every speech request of every path (recording reads the accepted
        // ones as speech events).
        std::uint32_t __fastcall SpeechCoreDetour(void* speechEntity, void* scope, void* params)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.audio_speech_core");
            InFlight inFlight;
            if (t_animScopeDepth)
                ++t_animSpeech;
            SpeechCoreCall call;
            if (params)
                GuardedRead(&call.requestedContext, static_cast<std::uint8_t*>(params) + kParamsContext, 4);
            // puppet_audio_rule.h RefusePuppetOwnSpeech: the puppet's own
            // speech is refused here the way the core's own first check refuses
            // (scope released, 0x3F, no line).
            std::uint32_t puppetOwner = 0;
            if (t_replayEmitDepth == 0 && PuppetSpeechOwner(reinterpret_cast<std::uint64_t>(speechEntity), puppetOwner))
            {
                const std::uintptr_t release = g_ownSpeechRelease.load(std::memory_order_acquire);
                const bool refuse = release && scope && replay::puppetaudio::RefusePuppetOwnSpeech(
                    g_puppetOwnSpeechBlock.load(std::memory_order_relaxed), true, false);
                PuppetOwnSpeech own;
                own.timeUs = QpcUs();
                own.owner = puppetOwner;
                own.context = call.requestedContext;
                own.refused = refuse;
                void* ownFrames[3] = {};
                const USHORT ownCaptured = RtlCaptureStackBackTrace(1, 3, ownFrames, nullptr);
                for (std::uint32_t depth = 0; depth < 3; ++depth)
                    own.callers[depth] = depth < ownCaptured ? ToRva(ownFrames[depth]) : 0;
                g_puppetOwnSpeechSeen.fetch_add(1, std::memory_order_relaxed);
                if (refuse)
                {
                    g_puppetOwnSpeechRefused.fetch_add(1, std::memory_order_relaxed);
                    own.result = replay::puppetaudio::kPuppetOwnSpeechRefusal;
                    replay::ring::Writer<PuppetOwnSpeech> ownSlot(g_ownSpeech, kOwnSpeechRing, g_nextOwnSpeech);
                    ownSlot.value = own;
                    ownSlot.Publish();
                    engine::Call(engine::kSpeechScopeRelease, release, scope);
                    return replay::puppetaudio::kPuppetOwnSpeechRefusal;
                }
                const SpeechCoreFn passOriginal = g_originalCore.load(std::memory_order_acquire);
                own.result = passOriginal ? passOriginal(speechEntity, scope, params) : 0;
                replay::ring::Writer<PuppetOwnSpeech> ownSlot(g_ownSpeech, kOwnSpeechRing, g_nextOwnSpeech);
                ownSlot.value = own;
                ownSlot.Publish();
                return own.result;
            }
            const bool worldDuringPlayback = t_replayEmitDepth == 0 &&
                g_puppetSpeechCount.load(std::memory_order_relaxed) > 0;
            auto* speech = static_cast<std::uint8_t*>(speechEntity);
            std::uint16_t linesBefore = 0;
            const bool countRead = speech && GuardedRead(&linesBefore, speech + 0x80, sizeof(linesBefore));
            const SpeechCoreFn original = g_originalCore.load(std::memory_order_acquire);
            const std::uint32_t result = original ? original(speechEntity, scope, params) : 0;
            std::uint16_t linesAfter = 0;
            std::uint8_t* list = nullptr;
            std::uint8_t* line = nullptr;
            std::uint8_t* data = nullptr;
            if (countRead && GuardedRead(&linesAfter, speech + 0x80, sizeof(linesAfter)) &&
                linesAfter > linesBefore && linesAfter <= 64 &&
                GuardedRead(&list, speech + 0x78, sizeof(list)) && list &&
                GuardedRead(&line, list + (linesAfter - 1u) * sizeof(line), sizeof(line)) && line &&
                GuardedRead(&data, line + 0x1D8, sizeof(data)) && data &&
                GuardedRead(&call.lineVoice, data + 0x110, 4) &&
                GuardedRead(&call.lineContext, data + 0x114, 4) &&
                GuardedRead(&call.lineVariation, data + 0x118, 4))
            {
                call.queued = true;
                call.line = reinterpret_cast<std::uint64_t>(line);
                GuardedRead(&call.lineState, line + 0x234, 4);
                GuardedRead(&call.dataType, data + 0xE0, 4);
                std::uint8_t* voiceObject = nullptr;
                if (GuardedRead(&voiceObject, line + 0x1E8, sizeof(voiceObject)) && voiceObject &&
                    GuardedRead(&call.voiceObjectType, voiceObject, 1) &&
                    GuardedRead(&call.voiceObjectPartial, voiceObject + 8, 4))
                {
                    call.voiceObject = reinterpret_cast<std::uint64_t>(voiceObject);
                }
                // speech_request.h: the line's copies of the resolved request,
                // one guarded read.
                std::uint8_t block[replay::speech::request::kLineBlockBytes] = {};
                if (GuardedRead(block, line + replay::speech::request::kLineBlockFrom, sizeof(block)))
                    replay::speech::request::FromLineBlock(block, sizeof(block), call.request);
                // speech_conversation.h: the line's sound set (data +0xE8 =
                // params +0xC0..+0xCF, copied by the line data init
                // (kDataCopiesSet) before the core returns).
                std::uint8_t setBlock[replay::speech::request::kDataSetBytes] = {};
                if (GuardedRead(setBlock, data + replay::speech::request::kDataSetFrom, sizeof(setBlock)))
                    replay::speech::request::SetFromDataBlock(setBlock, sizeof(setBlock), call.request);
                std::uint8_t* attached = nullptr;
                call.scopeHoldsLine = scope &&
                    GuardedRead(&attached, static_cast<std::uint8_t*>(scope) + 0x28, sizeof(attached)) &&
                    attached == line;
            }
            if (worldDuringPlayback)
            {
                g_worldSpeechDuringPlayback.fetch_add(1, std::memory_order_relaxed);
                if (call.queued)
                    g_worldSpeechQueuedDuringPlayback.fetch_add(1, std::memory_order_relaxed);
            }
            void* frames[4] = {};
            const USHORT captured = RtlCaptureStackBackTrace(1, 4, frames, nullptr);
            call.timeUs = QpcUs();
            call.speechEntity = reinterpret_cast<std::uint64_t>(speechEntity);
            call.result = result;
            for (std::uint32_t depth = 0; depth < 4; ++depth)
                call.callers[depth] = depth < captured ? ToRva(frames[depth]) : 0;
            replay::ring::Writer<SpeechCoreCall> slot(g_core, kCoreRing, g_nextCore);
            slot.value = call;
            slot.Publish();
            return result;
        }

        bool GuardedCopy(void* out, const void* source, std::size_t size)
        {
            // See GuardedRead (guarded_memory.h).
            return replay::mem::Copy(out, source, size, replay::mem::kGameHeapLowest);
        }

        void __fastcall ExplosionEnqueueDetour(void* manager, const void* request)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.audio_explosion");
            InFlight inFlight;
            std::int32_t before = -1;
            const bool readBefore = manager && GuardedCopy(&before, static_cast<const std::uint8_t*>(manager) + 0x400, 4);
            const ExplosionEnqueueFn original = g_originalExplosion.load(std::memory_order_acquire);
            if (original)
                original(manager, request);
            std::int32_t after = -1;
            if (t_playingExplosion)
            {
                const bool readAfter = readBefore &&
                    GuardedCopy(&after, static_cast<const std::uint8_t*>(manager) + 0x400, 4);
                t_playQueued = readAfter && after == before + 1 ? 1 : 0;
                t_playQueueBefore = before;
                return;
            }
            if (!g_enabled.load(std::memory_order_relaxed) || !readBefore ||
                !GuardedCopy(&after, static_cast<const std::uint8_t*>(manager) + 0x400, 4) || after != before + 1 ||
                before < 0 || before >= 16)
                return;
            // The entry the enqueuer stored (accepted), not the argument.
            std::uint8_t stored[0x40] = {};
            if (!GuardedCopy(stored, static_cast<const std::uint8_t*>(manager) + static_cast<std::size_t>(before) * 0x40, sizeof(stored)))
                return;
            ExplosionAudioCall call;
            call.timeUs = QpcUs();
            std::memcpy(call.position, stored + 0x20, sizeof(call.position));
            call.underwater = stored[0x30];
            call.b31 = stored[0x31];
            call.b32 = stored[0x32];
            std::memcpy(&call.interior, stored + 0x34, sizeof(call.interior));
            std::uint64_t instance = 0;
            std::memcpy(&instance, stored + 0x38, sizeof(instance));
            if (instance)
            {
                std::int32_t tag = -1;
                std::uint32_t overrideRef = 0;
                if (GuardedCopy(&tag, reinterpret_cast<const void*>(instance), 4) &&
                    GuardedCopy(&overrideRef, reinterpret_cast<const void*>(instance + 0x794), 4))
                {
                    call.hasInstance = true;
                    call.tag = tag;
                    call.overrideRef = overrideRef;
                }
            }
            void* frames[3] = {};
            const USHORT captured = RtlCaptureStackBackTrace(1, 3, frames, nullptr);
            const std::uintptr_t address = captured > 0 ? reinterpret_cast<std::uintptr_t>(frames[0]) : 0;
            call.caller = address >= g_base && address < g_imageEnd ? static_cast<std::uint32_t>(address - g_base) : 0;
            replay::ring::Writer<ExplosionAudioCall> slot(g_explosions, kExplosionRing, g_nextExplosion);
            slot.value = call;
            slot.Publish();
        }

        void __fastcall PedAnimHandlerDetour(void* audio)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.audio_anim");
            InFlight inFlight;
            AnimAudioBatch batch;
            const auto* bytes = static_cast<const std::uint8_t*>(audio);
            std::int32_t count = 0;
            const bool read = audio && GuardedCopy(&count, bytes + 0xD8, 4) && count > 0 && count <= 4 &&
                GuardedCopy(&batch.owner, bytes + 0x4600, 8);
            if (read)
            {
                batch.count = static_cast<std::uint32_t>(count);
                for (std::int32_t n = 0; n < count; ++n)
                {
                    GuardedCopy(&batch.extra[n], bytes + 0x78 + n * 0x18, 4);
                    GuardedCopy(&batch.hash[n], bytes + 0x78 + n * 0x18 + 4, 4);
                    // The rest of the event, expected zero: the word at +8 and
                    // the qword at +0x10 only; +0xA..+0xF is padding the MoVE
                    // callback never writes (it ands the word at [rbp-0x18]).
                    std::uint16_t word = 1;
                    std::uint64_t qword = 1;
                    if (GuardedCopy(&word, bytes + 0x78 + n * 0x18 + 8, sizeof(word)) &&
                        GuardedCopy(&qword, bytes + 0x78 + n * 0x18 + 0x10, sizeof(qword)) && (word || qword))
                        ++batch.tailNonZero;
                }
            }
            ++t_animScopeDepth;
            const std::uint32_t sounds = t_animSounds, footsteps = t_animFootsteps, speech = t_animSpeech;
            const PedAnimHandlerFn original = g_originalPedAnim.load(std::memory_order_acquire);
            if (original)
                original(audio);
            --t_animScopeDepth;
            if (!read || !g_enabled.load(std::memory_order_relaxed))
                return;
            batch.timeUs = QpcUs();
            batch.audio = reinterpret_cast<std::uint64_t>(audio);
            batch.sounds = t_animSounds - sounds;
            batch.footsteps = t_animFootsteps - footsteps;
            batch.speech = t_animSpeech - speech;
            replay::ring::Writer<AnimAudioBatch> slot(g_animBatches, kAnimRing, g_nextAnimBatch);
            slot.value = batch;
            slot.Publish();
        }

        void __fastcall SpeechLineStopDetour(void* line, std::uint32_t reason)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.audio_speech_stop");
            InFlight inFlight;
            SpeechLineStop stop;
            stop.timeUs = QpcUs();
            stop.line = reinterpret_cast<std::uint64_t>(line);
            stop.reason = reason;
            if (line)
            {
                auto* bytes = static_cast<std::uint8_t*>(line);
                GuardedRead(&stop.state, bytes + 0x234, 4);
                // The state 2 checks' fields.
                std::uint8_t* data = nullptr;
                stop.fieldsRead = GuardedRead(&stop.slot, bytes + 0xD0, 8) &&
                    GuardedRead(&stop.flags2BE, bytes + 0x2BE, 1) &&
                    GuardedRead(&stop.voiceObject, bytes + 0x1E8, 8);
                GuardedRead(&stop.source, bytes + 0x218, 4);       // closed theater, E1/E2
                GuardedRead(&stop.explicitSlot, bytes + 0x208, 8);
                if (GuardedRead(&data, bytes + 0x1D8, sizeof(data)) && data)
                {
                    GuardedRead(&stop.dataType, data + 0xE0, 4);
                    if (GuardedRead(&stop.dataSound, data + 0xC0, 8) && stop.dataSound)
                        GuardedRead(&stop.soundFlagsEE, reinterpret_cast<const void*>(stop.dataSound + 0xEE), 1);
                }
                // The owner, as the engine reads it on line +0xC0: r = [+0xD0];
                // o = [r+0x20]; parent = [o+0x18] when [o+0x10] == r.
                std::uint64_t owner = 0;
                std::uint64_t back = 0;
                stop.parentRead = true;
                if (stop.slot && GuardedRead(&owner, reinterpret_cast<const void*>(stop.slot + 0x20), 8) && owner &&
                    GuardedRead(&back, reinterpret_cast<const void*>(owner + 0x10), 8) && back == stop.slot)
                    GuardedRead(&stop.parent, reinterpret_cast<const void*>(owner + 0x18), 8);
            }
            void* frames[4] = {};
            const USHORT captured = RtlCaptureStackBackTrace(1, 4, frames, nullptr);
            for (std::uint32_t depth = 0; depth < 4; ++depth)
                stop.callers[depth] = depth < captured ? ToRva(frames[depth]) : 0;
            replay::ring::Writer<SpeechLineStop> slot(g_stops, kStopRing, g_nextStop);
            slot.value = stop;
            slot.Publish();
            const SpeechLineStopFn original = g_originalStop.load(std::memory_order_acquire);
            if (original)
                original(line, reason);
        }

        // Read-only: forwards first, then records the line and its state.
        void __fastcall SpeechLineAudibleDetour(void* line)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.audio_speech_start");
            InFlight inFlight;
            SpeechLineStart start;
            start.timeUs = QpcUs();
            start.line = reinterpret_cast<std::uint64_t>(line);
            const SpeechLineAudibleFn original = g_originalAudible.load(std::memory_order_acquire);
            if (original)
                original(line);
            if (line)
            {
                auto* bytes = static_cast<std::uint8_t*>(line);
                GuardedRead(&start.stateAfter, bytes + 0x234, 4);
                // The identity the recorder checks.
                std::uint8_t* data = nullptr;
                start.identityRead = GuardedRead(&data, bytes + 0x1D8, sizeof(data)) && data &&
                    GuardedRead(&start.voice, data + 0x110, 4) &&
                    GuardedRead(&start.context, data + 0x114, 4) &&
                    GuardedRead(&start.variation, data + 0x118, 4);
            }
            replay::ring::Writer<SpeechLineStart> slot(g_starts, kStartRing, g_nextStart);
            slot.value = start;
            slot.Publish();
        }

        void __fastcall FireEnqueueDetour(const void* info)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.audio_fire");
            InFlight inFlight;
            if (g_enabled.load(std::memory_order_relaxed) && info)
            {
                replay::ring::Writer<FireCall> slot(g_fires, kFireRing, g_nextFire);
                slot.value.timeUs = QpcUs();
                if (!GuardedRead(slot.value.info, info, kFireInfoBytes))
                    std::memset(slot.value.info, 0, kFireInfoBytes);
                slot.Publish();
            }
            const FireEnqueueFn original = g_originalFire.load(std::memory_order_acquire);
            if (original)
                original(info);
        }

        void RecordMelee(std::uint32_t entry, void* a, void* b, void* c, void* d)
        {
            void* frames[3] = {};
            const USHORT captured = RtlCaptureStackBackTrace(2, 3, frames, nullptr);
            replay::ring::Writer<MeleeCall> slot(g_melee, kMeleeRing, g_nextMelee);
            MeleeCall& call = slot.value;
            call.timeUs = QpcUs();
            call.entry = entry;
            call.args[0] = reinterpret_cast<std::uint64_t>(a);
            call.args[1] = reinterpret_cast<std::uint64_t>(b);
            call.args[2] = reinterpret_cast<std::uint64_t>(c);
            call.args[3] = reinterpret_cast<std::uint64_t>(d);
            if (!b || !GuardedRead(call.info, b, sizeof(call.info)))
                std::memset(call.info, 0, sizeof(call.info));
            for (std::uint32_t depth = 0; depth < 3; ++depth)
                call.callers[depth] = depth < captured ? ToRva(frames[depth]) : 0;
            slot.Publish();
        }
        constexpr std::uint32_t kKnownCategories = 64;
        std::atomic<std::uint64_t> g_knownCategories[kKnownCategories] = {};

        void NoteSoundCategory(const std::uint8_t* params)
        {
            std::uint64_t category = 0;
            std::memcpy(&category, params + kSoundCategoryOffset, sizeof(category));
            if (!category || (category & 7u) != 0)
                return;
            for (std::atomic<std::uint64_t>& known : g_knownCategories)
            {
                std::uint64_t value = known.load(std::memory_order_acquire);
                if (value == category)
                    return;
                if (!value && known.compare_exchange_strong(value, category, std::memory_order_acq_rel))
                    return;
                if (value == category)
                    return;
            }
        }
    }

        bool KnownSoundCategory(std::uint64_t category)
        {
            if (!category)
                return false;
            for (const std::atomic<std::uint64_t>& known : g_knownCategories)
            {
                const std::uint64_t value = known.load(std::memory_order_acquire);
                if (value == category)
                    return true;
                if (!value)
                    return false;
            }
            return false;
        }
    namespace
    {

        // The verdict of a puppet's own create, and whether the port refuses
        // it (S04, S05, S06; S03 refuses its one-shots anyway).
        replay::soundowner::LifeVerdict JudgeOwn(std::uint32_t owner, std::uint64_t soundRef, bool& refuse)
        {
            const replay::soundowner::PacketLives* lives = g_blockLives.load(std::memory_order_acquire);
            const replay::soundowner::LifeVerdict verdict = lives
                ? lives->Judge(owner, soundRef, g_blockPlayheadMs.load(std::memory_order_relaxed))
                : replay::soundowner::LifeVerdict::Passed;
            // the hidden player and his mount (no clip entity) stay silent
            refuse = !lives || owner == replay::puppetaudio::kNoOwner || g_blockAll.load(std::memory_order_relaxed) ||
                verdict != replay::soundowner::LifeVerdict::Passed;
            return verdict;
        }
    }

        // One own create of packets mode into the ring, with its verdict
        // (refused or passed).
        void NoteBlocked(std::uint8_t port, std::uint32_t owner, std::uint64_t soundRef, std::uint32_t caller,
            replay::soundowner::LifeVerdict verdict = replay::soundowner::LifeVerdict::Passed, bool refused = true)
        {
            if (refused)
                g_blockedPuppetCreates.fetch_add(1, std::memory_order_relaxed);
            replay::ring::Writer<BlockedPuppetCreate> slot(g_blocked, kBlockedRing, g_nextBlocked);
            slot.value.timeUs = QpcUs();
            slot.value.ref = soundRef;
            slot.value.owner = owner;
            slot.value.caller = caller;
            slot.value.port = port;
            slot.value.verdict = static_cast<std::uint8_t>(verdict);
            slot.value.refused = refused;
            slot.Publish();
        }
    namespace
    {

        // Whether packets mode refuses this create (and notes it).
        bool BlockedInPackets(std::uint8_t port, std::uint64_t audio, std::uint64_t soundRef, std::uint32_t caller)
        {
            if (!g_blockPuppetSounds.load(std::memory_order_relaxed) ||
                g_silencedPuppetAudioCount.load(std::memory_order_relaxed) == 0)
                return false;
            std::uint32_t owner = replay::puppetaudio::kNoOwner;
            if (!replay::puppetaudio::BlockPuppetCreate(true, SilencedPuppetAudioOwner(audio, owner), t_replayEmitDepth > 0,
                    t_replayedSystemDepth > 0, caller, Callers().speechController))
                return false;
            bool refuse = true;
            const replay::soundowner::LifeVerdict verdict = JudgeOwn(owner, soundRef, refuse);
            NoteBlocked(port, owner, soundRef, caller, verdict, refuse);
            return refuse;
        }

        bool __fastcall SoundRequestDetour(std::uint64_t audio, std::uint64_t soundRef, std::uint64_t owner,
            std::uint64_t params, std::uint64_t f1, std::uint64_t f2, std::uint64_t f3, std::uint64_t f4)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            if (g_enabled.load(std::memory_order_relaxed))
            {
                void* frames[2] = {};
                const USHORT captured = RtlCaptureStackBackTrace(1, 2, frames, nullptr);
                replay::ring::Writer<SoundRequest> slot(g_soundRequests, kSoundRequestRing, g_nextSoundRequest);
                SoundRequest& request = slot.value;
                request.timeUs = QpcUs();
                request.audioEntity = audio;
                request.soundRef = soundRef;
                request.owner = owner;
                request.inReplayedSystem = t_replayedSystemDepth > 0;
                request.hasParams = params &&
                    GuardedRead(request.params, reinterpret_cast<void*>(params), sizeof(request.params));
                if (request.hasParams)
                    NoteSoundCategory(request.params);
                request.flags[0] = f1;
                request.flags[1] = f2;
                request.flags[2] = f3;
                request.flags[3] = f4;
                for (std::uint32_t depth = 0; depth < 2; ++depth)
                    request.callers[depth] = depth < captured ? ToRva(frames[depth]) : 0;
                slot.Publish();
            }
            if (replay::soundfx::Active())
                replay::soundfx::NoteCall(replay::soundfx::kS04, _ReturnAddress());
            // Packets mode: a puppet's own deferred request.
            if (BlockedInPackets(4, audio, soundRef, ToRva(_ReturnAddress())))
                return false;
            const SoundRequestFn original = g_originalSoundRequest.load(std::memory_order_acquire);
            const bool accepted = original ? original(audio, soundRef, owner, params, f1, f2, f3, f4) : false;
            // The generic sound recorder's request (the deferred queue), with
            // the caller path of this instant.
            if (accepted && replay::soundgeneric::Capturing())
            {
                void* above[4] = {};
                const USHORT got = RtlCaptureStackBackTrace(1, 4, above, nullptr);
                replay::soundgeneric::OnEnqueued(replay::soundgeneric::Source::Deferred, audio, soundRef, QpcUs(), above, got);
            }
            return accepted;
        }

        // Read-only: forwards first and records only what the engine
        // accepted. A refused request (an empty entry of an indirect sound
        // set, an invalid ref) made no sound in the game either; recorded, it
        // would come back as a playback refusal.
        bool __fastcall CreateAndPlayDetour(std::uint64_t audio, std::uint64_t soundRef, std::uint64_t params)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            // The read probe's tap, before the original (the params are still
            // the caller's) and after it.
            const bool probed = replay::soundfx::Active();
            if (probed)
                replay::soundfx::NoteEnqueueRequest(audio, soundRef, params, _ReturnAddress());
            // The params are copied before the original may consume them.
            alignas(16) std::uint8_t copied[sizeof(SoundRequest::params)] = {};
            const bool enabled = g_enabled.load(std::memory_order_relaxed);
            // `ring` = recorded into the play ring (the evidence flag adds the
            // playback); `enabled` keeps its meaning elsewhere.
            const bool ring = enabled || g_playRequestEvidence.load(std::memory_order_relaxed) ||
                g_playRequestMixProbe.load(std::memory_order_relaxed);
            const bool hasParams = ring && params && GuardedRead(copied, reinterpret_cast<void*>(params), sizeof(copied));
            void* frames[4] = {};
            const USHORT captured = ring ? RtlCaptureStackBackTrace(1, 4, frames, nullptr) : 0;
            const std::uint64_t timeUs = QpcUs();
            const bool replayed = t_replayedSystemDepth > 0;
            // A puppet's own one-shot while a clip plays (the replay plays the
            // recorded SND events); the core's own refusal. Also on a weapon
            // puppet's audio (SND kind 4 carries it).
            const bool onPuppetWeapon = g_silencedPuppetWeaponCount.load(std::memory_order_relaxed) != 0 &&
                t_replayEmitDepth == 0 && !replayed && IsSilencedPuppetWeaponAudio(audio);
            if (onPuppetWeapon)
                g_refusedPuppetWeaponOneShots.fetch_add(1, std::memory_order_relaxed);
            if (onPuppetWeapon ||
                replay::puppetaudio::RefuseOwnOneShot(g_silencedPuppetAudioCount.load(std::memory_order_relaxed) != 0,
                    t_replayEmitDepth > 0, replayed, IsSilencedPuppetAudio(audio)))
            {
                const std::uint64_t refused = g_refusedPuppetOneShots.fetch_add(1, std::memory_order_relaxed) -
                    g_refusedOneShotBase.load(std::memory_order_relaxed);
                if (refused < kRefusedOneShotSamples)
                {
                    void* callers[2] = {};
                    const USHORT got = RtlCaptureStackBackTrace(1, 2, callers, nullptr);
                    g_refusedOneShotRef[refused].store(soundRef, std::memory_order_relaxed);
                    g_refusedOneShotCaller[refused].store(got > 1 ? ToRva(callers[1]) : got ? ToRva(callers[0]) : 0,
                        std::memory_order_relaxed);
                }
                // In packets mode the same refusal counts as blocked from the puppet.
                if (g_blockPuppetSounds.load(std::memory_order_relaxed))
                {
                    std::uint32_t owner = replay::puppetaudio::kNoOwner;
                    SilencedPuppetAudioOwner(audio, owner);
                    bool refuse = true; // S03 refuses the own one-shots anyway; the verdict is for the report
                    NoteBlocked(3, owner, soundRef, ToRva(_ReturnAddress()), JudgeOwn(owner, soundRef, refuse), true);
                }
                return false;
            }
            // Inside a replayed step or melee call, a set the owner never made
            // in the clip (the puppet's own branch).
            if (replayed && g_blockPuppetSounds.load(std::memory_order_relaxed) &&
                g_silencedPuppetAudioCount.load(std::memory_order_relaxed) != 0)
            {
                const replay::soundowner::PacketLives* lives = g_blockLives.load(std::memory_order_acquire);
                std::uint32_t owner = replay::puppetaudio::kNoOwner;
                const bool onPuppet = lives && SilencedPuppetAudioOwner(audio, owner);
                if (replay::puppetaudio::RefuseUnmadeReplayedSet(true, onPuppet,
                        replayed, onPuppet && lives->OwnerHasPackets(owner), onPuppet && lives->OwnerMadeSet(owner, soundRef)))
                {
                    g_refusedUnmadeSets.fetch_add(1, std::memory_order_relaxed);
                    NoteBlocked(3, owner, soundRef, ToRva(_ReturnAddress()), replay::soundowner::LifeVerdict::Unmade, true);
                    return false;
                }
            }
            const CreateAndPlayFn original = g_originalCreateAndPlay.load(std::memory_order_acquire);
            const bool result = original ? original(audio, soundRef, params) : false;
            if (probed)
                replay::soundfx::NoteEnqueueResult(audio, soundRef, result);
            // The generic sound recorder's request: the create event is born
            // here, with this instant and this caller path (at the creator the
            // caller is the queue's drain for most sounds).
            if (result && replay::soundgeneric::Capturing())
            {
                void* above[4] = {};
                const USHORT got = RtlCaptureStackBackTrace(1, 4, above, nullptr);
                replay::soundgeneric::OnEnqueued(replay::soundgeneric::Source::Queue, audio, soundRef, timeUs, above, got);
            }
            if (!result && enabled)
                g_refusedPlayRequests.fetch_add(1, std::memory_order_relaxed);
            if (ring && result)
            {
                replay::ring::Writer<SoundRequest> slot(g_playRequests, kSoundRequestRing, g_nextPlayRequest);
                SoundRequest& request = slot.value;
                request.timeUs = timeUs;
                request.audioEntity = audio;
                request.soundRef = soundRef;
                request.owner = 0;
                request.inReplayedSystem = replayed;
                request.hasParams = hasParams;
                if (hasParams)
                {
                    std::memcpy(request.params, copied, sizeof(request.params));
                    NoteSoundCategory(copied);
                }
                request.flags[0] = request.flags[1] = request.flags[2] = request.flags[3] = 0;
                for (std::uint32_t depth = 0; depth < 4; ++depth)
                    request.callers[depth] = depth < captured ? ToRva(frames[depth]) : 0;
                slot.Publish();
            }
            return result;
        }

        // Recorded into the direct ring with flags[0] = 1 (persistent).
        std::uint64_t __fastcall PersistentCreateDetour(std::uint64_t audio, std::uint64_t descriptor,
            std::uint64_t out, std::uint64_t params)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            const PersistentCreateFn original = g_originalPersistentCreate.load(std::memory_order_acquire);
            const std::uint64_t result = original ? original(audio, descriptor, out, params) : 0;
            if (g_enabled.load(std::memory_order_relaxed))
            {
                void* frames[4] = {};
                const USHORT captured = RtlCaptureStackBackTrace(1, 4, frames, nullptr);
                replay::ring::Writer<SoundRequest> slot(g_playRequests, kSoundRequestRing, g_nextPlayRequest);
                SoundRequest& request = slot.value;
                request.timeUs = QpcUs();
                request.audioEntity = audio;
                request.soundRef = 0;
                if (descriptor)
                    GuardedRead(&request.soundRef, reinterpret_cast<std::uint8_t*>(descriptor) + 0xC, 8);
                request.owner = 0;
                request.inReplayedSystem = t_replayedSystemDepth > 0;
                request.hasParams = params &&
                    GuardedRead(request.params, reinterpret_cast<void*>(params), sizeof(request.params));
                request.flags[0] = 1;
                request.flags[1] = request.flags[2] = request.flags[3] = 0;
                for (std::uint32_t depth = 0; depth < 4; ++depth)
                    request.callers[depth] = depth < captured ? ToRva(frames[depth]) : 0;
                request.slot = out;
                request.sound = 0;
                if (out)
                    GuardedRead(&request.sound, reinterpret_cast<void*>(out), sizeof(request.sound));
                const std::uint64_t collision = sig::Find(kStaticCollisionAudio);
                if (request.sound && collision && audio == collision)
                {
                    const std::uint32_t tracked = g_nextTracked.fetch_add(1, std::memory_order_relaxed);
                    g_trackedSounds[tracked % kTrackedSounds].store(request.sound, std::memory_order_release);
                }
                slot.Publish();
            }
            return result;
        }

        // Read-only, forwards first; records a create whose params carry a
        // tracker (an entity) and that the collision entry did not make. Also
        // a create without tracker on an audio entity outside the image (a
        // ped's audio: foley, weapon handling, the shell after a shot), while
        // recording or while a playback watches what its puppets create by
        // themselves (SetObserveEntityCreates).
        void RecordEntitySound(std::uint64_t audio, std::uint64_t soundRef, bool byHash, std::uint64_t out,
            std::uint64_t params, void* returnAddress)
        {
            if ((!g_enabled.load(std::memory_order_relaxed) && !g_observeEntityCreates.load(std::memory_order_relaxed)) ||
                !out)
                return;
            const std::uint32_t caller = ToRva(returnAddress);
            if (caller && caller == Callers().collisionCreate)
                return;
            std::uint64_t tracker = 0;
            if (params && !GuardedRead(&tracker, reinterpret_cast<std::uint8_t*>(params) + 0xC8, sizeof(tracker)))
                return;
            if (!tracker && (audio >= g_base && audio < g_imageEnd))
                return; // a static audio entity without tracker: no owner to find
            std::uint64_t sound = 0;
            GuardedRead(&sound, reinterpret_cast<void*>(out), sizeof(sound));
            if (!sound)
                return;
            replay::ring::Writer<EntitySoundCreate> slot(g_entitySounds, kEntitySoundRing, g_nextEntitySound);
            EntitySoundCreate& create = slot.value;
            create.timeUs = QpcUs();
            create.audio = audio;
            create.soundRef = soundRef;
            create.byHash = byHash;
            create.slot = out;
            create.sound = sound;
            create.tracker = tracker;
            create.hasParams = params && GuardedRead(create.params, reinterpret_cast<void*>(params), sizeof(create.params));
            if (create.hasParams)
                NoteSoundCategory(create.params);
            create.caller = caller;
            slot.Publish();
            if (!g_enabled.load(std::memory_order_relaxed))
                return; // playback watch: no stop tracking
            const std::uint32_t tracked = g_nextEntityTracked.fetch_add(1, std::memory_order_relaxed);
            g_entityTracked[tracked % kEntityTracked].store(sound, std::memory_order_release);
        }

        // The two return addresses above the call site (the game code that
        // asked the wrapper or the core), for the samples only.
        // RtlCaptureStackBackTrace walks the .pdata unwind data; no native.
        void GameFramesAbove(std::uint32_t caller, std::atomic<std::uint32_t> (&out)[2])
        {
            out[0].store(0, std::memory_order_relaxed);
            out[1].store(0, std::memory_order_relaxed);
            if (!caller)
                return; // a call from outside the image: nothing to find
            void* frames[12] = {};
            const USHORT captured = RtlCaptureStackBackTrace(0, 12, frames, nullptr);
            for (USHORT n = 0; n + 1 < captured; ++n)
            {
                if (ToRva(frames[n]) != caller)
                    continue;
                out[0].store(ToRva(frames[n + 1]), std::memory_order_relaxed);
                out[1].store(n + 2 < captured ? ToRva(frames[n + 2]) : 0, std::memory_order_relaxed);
                return;
            }
        }

        // The creates the generic player asked the game's queue for (mode c):
        // the queue's own create of that (audio, ref) passes the port block
        // once, within its time. Script thread writes, any thread takes.
        constexpr std::size_t kExpectedQueued = 32;
        std::atomic<std::uint64_t> g_expectedAudio[kExpectedQueued] = {};
        std::atomic<std::uint64_t> g_expectedRef[kExpectedQueued] = {};
        std::atomic<std::uint64_t> g_expectedUntilUs[kExpectedQueued] = {};
        std::atomic<std::uint32_t> g_expectedNext{0};
    }

        void ExpectQueuedCreate(std::uint64_t audio, std::uint64_t soundRef)
        {
            const std::size_t n = g_expectedNext.fetch_add(1, std::memory_order_relaxed) % kExpectedQueued;
            g_expectedAudio[n].store(0, std::memory_order_release);
            g_expectedRef[n].store(soundRef, std::memory_order_relaxed);
            g_expectedUntilUs[n].store(QpcUs() + 2000000ull, std::memory_order_relaxed);
            g_expectedAudio[n].store(audio, std::memory_order_release);
        }
    namespace
    {

        bool TakeExpectedQueuedCreate(std::uint64_t audio, std::uint64_t soundRef)
        {
            if (!audio)
                return false;
            const std::uint64_t now = QpcUs();
            for (std::size_t n = 0; n < kExpectedQueued; ++n)
            {
                std::uint64_t expected = g_expectedAudio[n].load(std::memory_order_acquire);
                if (expected != audio || g_expectedRef[n].load(std::memory_order_relaxed) != soundRef ||
                    g_expectedUntilUs[n].load(std::memory_order_relaxed) < now)
                    continue;
                if (g_expectedAudio[n].compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
                    return true;
            }
            return false;
        }
    }
        std::atomic<std::uint64_t> g_ticketAudio[kQueueTickets] = {};
        std::atomic<std::uint64_t> g_ticketRef[kQueueTickets] = {};
        std::atomic<std::uint64_t> g_ticketUntilUs[kQueueTickets] = {};
        std::atomic<std::uint64_t> g_ticketId[kQueueTickets] = {};
    namespace
    {
        std::atomic<std::uint64_t> g_ticketNext{0};
    }
        std::atomic<std::uint32_t> g_ticketsWaiting{0};
        QueueBirthSlot g_queueBirths[kQueueBirthRing];
        std::atomic<std::uint64_t> g_queueBirthNext{0};

        std::uint64_t OpenQueueTicket(std::uint64_t audio, std::uint64_t soundRef)
        {
            const std::uint64_t id = g_ticketNext.fetch_add(1, std::memory_order_relaxed) + 1;
            const std::size_t n = static_cast<std::size_t>(id % kQueueTickets);
            g_ticketAudio[n].store(0, std::memory_order_release);
            g_ticketId[n].store(id, std::memory_order_relaxed);
            g_ticketRef[n].store(soundRef, std::memory_order_relaxed);
            g_ticketUntilUs[n].store(QpcUs() + 2000000ull, std::memory_order_relaxed);
            g_ticketAudio[n].store(audio, std::memory_order_release);
            g_ticketsWaiting.store(1, std::memory_order_relaxed);
            return id;
        }
    namespace
    {

        // puppet_audio_rule.h: a puppet's own persistent sound without tracker
        // from a call site the clip's ESND track carries. Refused as the core
        // refuses an unregistered audio entity (by ref and by hash: *out = 0,
        // nothing created). Carried means the same entity,
        // call site and sound entry in the clip (OwnPersistentCreateCarried;
        // the call site alone on the hidden player and his mount).
        bool RefusePuppetOwnPersistentCreate(std::uint64_t audio, std::uint64_t soundRef, bool byHash, std::uint64_t out,
            std::uint64_t params, void* returnAddress)
        {
            // Packets mode refuses every own create of a silenced holder
            // (tracker or not, carried or not), the speech's sound controller
            // apart; the ini RefusePuppetOwnPersistent is for the legacy mode.
            // The generic player's request through the game's queue (mode c) is
            // not a puppet's own, but only with out = 0: mode c's sound is a
            // play-and-forget one the queue makes with no persistent reference.
            // A create with out is a persistent wrapper's, the puppet's own;
            // letting it take the expectation (and the ticket) of our request
            // for the same sound leaves our sound with no ticket, never stopped,
            // and counted as another source's.
            if (!out && TakeExpectedQueuedCreate(audio, soundRef))
                return false;
            if (out && g_blockPuppetSounds.load(std::memory_order_relaxed))
            {
                std::uint32_t blockedOwner = replay::puppetaudio::kNoOwner;
                const std::uint32_t site = ToRva(returnAddress);
                if (g_silencedPuppetAudioCount.load(std::memory_order_relaxed) == 0 ||
                    !replay::puppetaudio::BlockPuppetCreate(true, SilencedPuppetAudioOwner(audio, blockedOwner),
                        t_replayEmitDepth > 0, t_replayedSystemDepth > 0, site, Callers().speechController))
                    return false;
                // Only what the recording plays (or a proved double) is refused;
                // the rest is left to the engine and counted.
                bool refuse = true;
                replay::soundowner::LifeVerdict verdict = JudgeOwn(blockedOwner, soundRef, refuse);
                // A persistent sound by ref the owner never made anywhere in the
                // clip and the ESND track does not carry: refused too.
                if (!refuse && !byHash)
                {
                    const replay::soundowner::PacketLives* lives = g_blockLives.load(std::memory_order_acquire);
                    const bool clipOwner = lives && blockedOwner != replay::puppetaudio::kNoOwner;
                    const bool madeSet = clipOwner && lives->OwnerMadeSet(blockedOwner, soundRef);
                    const bool carried = clipOwner && !madeSet && OwnPersistentCreateCarried(blockedOwner, site, soundRef, false);
                    if (replay::puppetaudio::RefuseUnrecordedOwnPersistent(true, true, clipOwner, madeSet, carried))
                    {
                        refuse = true;
                        verdict = replay::soundowner::LifeVerdict::Unrecorded;
                        g_unrecordedRefused.fetch_add(1, std::memory_order_relaxed);
                        const std::uint32_t n = g_unrecordedSampleCount.load(std::memory_order_relaxed);
                        if (n < kUnrecordedSamples)
                        {
                            bool seen = false; // one sample per (owner, ref)
                            for (std::uint32_t k = 0; k < n && !seen; ++k)
                                seen = g_unrecordedOwner[k].load(std::memory_order_relaxed) == blockedOwner &&
                                    g_unrecordedRef[k].load(std::memory_order_relaxed) == soundRef;
                            std::uint32_t expected = n;
                            if (!seen && g_unrecordedSampleCount.compare_exchange_strong(expected, n + 1, std::memory_order_relaxed))
                            {
                                g_unrecordedOwner[n].store(blockedOwner, std::memory_order_relaxed);
                                g_unrecordedRef[n].store(soundRef, std::memory_order_relaxed);
                                g_unrecordedSite[n].store(site, std::memory_order_relaxed);
                            }
                        }
                    }
                    else if (clipOwner && !madeSet && carried)
                        g_unrecordedKeptByEsnd.fetch_add(1, std::memory_order_relaxed);
                }
                if (!refuse)
                {
                    NoteBlocked(byHash ? 6 : 5, blockedOwner, soundRef, site, verdict, false);
                    return false;
                }
                const std::uint64_t none = 0;
                if (!replay::mem::Put(out, &none, sizeof(none)))
                    return false; // the slot is not writable: left to the engine
                NoteBlocked(byHash ? 6 : 5, blockedOwner, soundRef, site, verdict, true);
                return true;
            }
            if (!out || !g_refuseOwnPersistent.load(std::memory_order_relaxed) ||
                g_silencedPuppetAudioCount.load(std::memory_order_relaxed) == 0)
                return false;
            std::uint32_t owner = replay::puppetaudio::kNoOwner;
            if (!SilencedPuppetAudioOwner(audio, owner))
                return false;
            std::uint64_t tracker = 0;
            if (params && !GuardedRead(&tracker, reinterpret_cast<std::uint8_t*>(params) + 0xC8, sizeof(tracker)))
                return false; // unreadable params: left to the engine
            const std::uint32_t caller = ToRva(returnAddress);
            const bool carried = OwnPersistentCreateCarried(owner, caller, soundRef, byHash);
            // The speech's sound controller is exempt here too
            // (puppet_audio_rule.h LegacyRefuseOwnPersistent).
            if (!replay::puppetaudio::LegacyRefuseOwnPersistent(caller, Callers().speechController, true,
                    t_replayEmitDepth > 0, t_replayedSystemDepth > 0, true, tracker != 0, carried))
            {
                if (!tracker && !carried && t_replayEmitDepth == 0 && t_replayedSystemDepth == 0)
                {
                    const std::uint64_t kept = g_keptOwnPersistent.fetch_add(1, std::memory_order_relaxed) -
                        g_keptOwnPersistentBase.load(std::memory_order_relaxed);
                    if (kept < kOwnPersistentSamples)
                    {
                        g_keptPersistentRef[kept].store(soundRef, std::memory_order_relaxed);
                        g_keptPersistentCaller[kept].store(caller, std::memory_order_relaxed);
                        g_keptPersistentOwner[kept].store(owner, std::memory_order_relaxed);
                        GameFramesAbove(caller, g_keptPersistentFrame[kept]);
                    }
                }
                return false;
            }
            const std::uint64_t zero = 0;
            if (!replay::mem::Put(out, &zero, sizeof(zero)))
                return false; // the slot is not writable: left to the engine
            const std::uint64_t refused = g_refusedOwnPersistent.fetch_add(1, std::memory_order_relaxed) -
                g_refusedOwnPersistentBase.load(std::memory_order_relaxed);
            if (refused < kOwnPersistentSamples)
            {
                g_refusedPersistentRef[refused].store(soundRef, std::memory_order_relaxed);
                g_refusedPersistentCaller[refused].store(caller, std::memory_order_relaxed);
                g_refusedPersistentOwner[refused].store(owner, std::memory_order_relaxed);
                GameFramesAbove(caller, g_refusedPersistentFrame[refused]);
            }
            return true;
        }

        void __fastcall PersistentByRefDetour(std::uint64_t audio, std::uint64_t soundRef, std::uint64_t out, std::uint64_t params)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            // The whole call, so the probe knows the sounds the speech makes inside it.
            replay::soundfx::ByRefScope probe(audio, soundRef, params, _ReturnAddress());
            if (RefusePuppetOwnPersistentCreate(audio, soundRef, false, out, params, _ReturnAddress()))
                return;
            const PersistentByRefFn original = g_originalByRef.load(std::memory_order_acquire);
            if (original)
                original(audio, soundRef, out, params);
            RecordEntitySound(audio, soundRef, false, out, params, _ReturnAddress());
        }

        void __fastcall PersistentByHashDetour(std::uint64_t audio, std::uint32_t hash, std::uint64_t out, std::uint64_t params)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            if (replay::soundfx::Active())
                replay::soundfx::NoteCall(replay::soundfx::kS06, _ReturnAddress());
            if (RefusePuppetOwnPersistentCreate(audio, hash, true, out, params, _ReturnAddress()))
                return;
            const PersistentByHashFn original = g_originalByHash.load(std::memory_order_acquire);
            if (original)
                original(audio, hash, out, params);
            RecordEntitySound(audio, hash, true, out, params, _ReturnAddress());
        }

        void __fastcall StopAndForgetDetour(std::uint64_t sound, std::uint8_t continueUpdating)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            if (replay::soundfx::Active()) // which sounds end with and without this call
                replay::soundfx::NoteForget(sound, _ReturnAddress());
            if (replay::soundgeneric::Capturing()) // the stop-and-forget event
                replay::soundgeneric::OnStop(sound, true);
            if (sound && g_enabled.load(std::memory_order_relaxed))
            {
                // The entity sounds' stops go to the same ring.
                for (std::atomic<std::uint64_t>& tracked : g_entityTracked)
                {
                    if (tracked.load(std::memory_order_acquire) != sound)
                        continue;
                    replay::ring::Writer<SoundStop> slot(g_soundStops, kStopRing2, g_nextSoundStop);
                    slot.value.timeUs = QpcUs();
                    slot.value.sound = sound;
                    slot.Publish();
                    tracked.store(0, std::memory_order_release);
                    break;
                }
                for (std::atomic<std::uint64_t>& tracked : g_trackedSounds)
                {
                    if (tracked.load(std::memory_order_acquire) != sound)
                        continue;
                    replay::ring::Writer<SoundStop> slot(g_soundStops, kStopRing2, g_nextSoundStop);
                    slot.value.timeUs = QpcUs();
                    slot.value.sound = sound;
                    slot.Publish();
                    tracked.store(0, std::memory_order_release);
                    break;
                }
            }
            const StopAndForgetFn original = g_originalStopAndForget.load(std::memory_order_acquire);
            if (original)
                original(sound, continueUpdating);
        }

        void __fastcall MeleeBodyImpactDetour(std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
            std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7, std::uint64_t a8,
            std::uint64_t a9, std::uint64_t a10)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            if (g_enabled.load(std::memory_order_relaxed))
            {
                void* frames[3] = {};
                const USHORT captured = RtlCaptureStackBackTrace(1, 3, frames, nullptr);
                replay::ring::Writer<ImpactCall> slot(g_impacts, kImpactRing, g_nextImpact);
                ImpactCall& call = slot.value;
                call = ImpactCall{};
                call.timeUs = QpcUs();
                const std::uint64_t args[10] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10};
                std::memcpy(call.args, args, sizeof(args));
                if (a2)
                    GuardedRead(call.position, reinterpret_cast<void*>(a2), sizeof(call.position));
                if (a3)
                    GuardedRead(call.object8, reinterpret_cast<void*>(a3), sizeof(call.object8));
                if (a4)
                    GuardedRead(call.object9, reinterpret_cast<void*>(a4), sizeof(call.object9));
                if (a6)
                {
                    GuardedRead(&call.definitionVtable, reinterpret_cast<void*>(a6), 8);
                    GuardedRead(&call.definitionHash, reinterpret_cast<std::uint8_t*>(a6) + 0x48, 4);
                }
                for (std::uint32_t depth = 0; depth < 3; ++depth)
                    call.callers[depth] = depth < captured ? ToRva(frames[depth]) : 0;
                slot.Publish();
            }
            const MeleeBodyImpactFn original = g_originalBodyImpact.load(std::memory_order_acquire);
            if (original)
                original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
        }

        void __fastcall MeleeImpactDetour(void* entity, void* info)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            InFlight inFlight;
            if (g_enabled.load(std::memory_order_relaxed))
                RecordMelee(1, entity, info, nullptr, nullptr);
            const MeleeImpactFn original = g_originalMeleeImpact.load(std::memory_order_acquire);
            ReplayedSystemScope scope;
            if (original)
                original(entity, info);
        }
    }

        // The prepare-and-play port: through the read probe's trampoline when
        // the probe hooked it (SoundFxProbe=1; its bytes then start with the
        // hook's jump, and the trampoline is the engine's function,
        // uncounted), else the function itself. Null when not in this game.
        void* PrepareAndPlayPort()
        {
            const std::uintptr_t at = sig::Find(kPrepareAndPlay);
            if (!at)
                return nullptr;
            void* probed = replay::soundfx::OriginalFor(at - sig::ImageBase());
            return probed ? probed : reinterpret_cast<void*>(at);
        }

    bool Install(std::string& error)
    {
        if (g_ctorTarget || g_sayTarget)
            return true;
        QueryPerformanceFrequency(&g_frequency);
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        g_base = reinterpret_cast<std::uintptr_t>(base);
        g_imageEnd = g_base + nt->OptionalHeader.SizeOfImage;
        const MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            error = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        Callers(); // resolved here, before any detour runs
        FootProof();
        Hook(kSoundParamsCtor, reinterpret_cast<void*>(&SoundParamsCtorDetour), g_originalCtor, g_ctorTarget,
            "sound_params", error);
        Hook(kSpeechSay, reinterpret_cast<void*>(&SpeechSayDetour), g_originalSay, g_sayTarget, "speech_say", error);
        if (const std::uintptr_t fire = sig::Find(kFireEnqueue))
            g_fireDirect = fire;
        Hook(kSpeechCore, reinterpret_cast<void*>(&SpeechCoreDetour), g_originalCore, g_coreTarget, "speech_core", error);
        // The scope release the core calls first, for the puppet's own speech
        // refusal.
        const std::uintptr_t scopeRelease = sig::Find(kSpeechScopeRelease);
        if (!g_coreTarget)
            g_ownSpeechError = "speech_core_not_hooked";
        else if (!scopeRelease)
            g_ownSpeechError = "scope_release_signature_mismatch";
        else
        {
            g_ownSpeechRelease.store(scopeRelease, std::memory_order_release);
            g_ownSpeechError = "";
        }
        Hook(kFootstep, reinterpret_cast<void*>(&FootstepDetour), g_originalFootstep, g_footstepTarget, "footstep", error);
        Hook(kPedAnimHandler, reinterpret_cast<void*>(&PedAnimHandlerDetour), g_originalPedAnim, g_pedAnimTarget,
            "ped_anim_events", error);
        Hook(kExplosionEnqueue, reinterpret_cast<void*>(&ExplosionEnqueueDetour), g_originalExplosion, g_explosionTarget,
            "explosion_enqueue", error);
        Hook(footevent::kMovementUpdate, reinterpret_cast<void*>(&MovementUpdateDetour), g_originalMovementUpdate,
            g_movementUpdateTarget, "movement_update", error);
        // The second queue's entry, after its store, generator and drain sites
        // (one missing leaves it off and the replay of the secondary steps with
        // it).
        if (!sig::Find(footevent::kEnqueueStore) || !sig::Find(footevent::kGeneratorCall) ||
            !sig::Find(footevent::kDrainCall))
        {
            g_footEventError = "foot_event_site_mismatch";
            error += "foot_event_site_mismatch;";
        }
        else if (Hook(footevent::kEnqueue, reinterpret_cast<void*>(&FootEventDetour), g_originalFootEvent,
                     g_footEventTarget, "foot_event", error))
            g_footEventError = "";
        else
            g_footEventError = "foot_event_hook_failed";
        Hook(kSpeechLineStop, reinterpret_cast<void*>(&SpeechLineStopDetour), g_originalStop, g_stopTarget,
            "speech_line_stop", error);
        Hook(kSpeechLineAudible, reinterpret_cast<void*>(&SpeechLineAudibleDetour), g_originalAudible, g_audibleTarget,
            "speech_line_start", error);
        Hook(kFireEnqueue, reinterpret_cast<void*>(&FireEnqueueDetour), g_originalFire, g_fireTarget, "fire_enqueue",
            error);
        Hook(kAudioMeleeImpact, reinterpret_cast<void*>(&MeleeImpactDetour), g_originalMeleeImpact, g_meleeImpactTarget,
            "melee_impact", error);
        // The melee body impact takes exactly ten arguments (both callers pass
        // ten; it reads its frame up to entry rsp+0x50 and no xmm argument):
        // the detour declares and forwards all ten. A four-argument detour
        // crashes the game (seen on a knife execution).
        Hook(kStopAndForget, reinterpret_cast<void*>(&StopAndForgetDetour), g_originalStopAndForget,
            g_stopAndForgetTarget, "stop_and_forget", error);
        Hook(kAudioPersistentCreate, reinterpret_cast<void*>(&PersistentCreateDetour), g_originalPersistentCreate,
            g_persistentCreateTarget, "persistent_create", error);
        Hook(kPersistentByRef, reinterpret_cast<void*>(&PersistentByRefDetour), g_originalByRef, g_byRefTarget,
            "persistent_by_ref", error);
        Hook(kPersistentByHash, reinterpret_cast<void*>(&PersistentByHashDetour), g_originalByHash, g_byHashTarget,
            "persistent_by_hash", error);
        Hook(kCreateAndPlay, reinterpret_cast<void*>(&CreateAndPlayDetour), g_originalCreateAndPlay,
            g_createAndPlayTarget, "create_and_play", error);
        Hook(kSoundRequest, reinterpret_cast<void*>(&SoundRequestDetour), g_originalSoundRequest, g_soundRequestTarget,
            "sound_request", error);
        Hook(kAudioMeleeBody, reinterpret_cast<void*>(&MeleeBodyImpactDetour), g_originalBodyImpact,
            g_bodyImpactTarget, "melee_body_impact", error);
        InstallPlayerWeaponAudio(error);
        InstallPlayerMountAudio(error);
        InstallMovementVolume(error);
        return g_ctorTarget || g_sayTarget;
    }

        bool GuardedPedAudio(void* ped, std::uint8_t*& audio)
        {
            static std::uintptr_t getter = 0;
            if (!getter)
            {
                getter = sig::Find(kPedAudioGetter);
                if (!getter)
                    return false;
            }
            return engine::Call(engine::kPedAudioGetter, getter, audio, ped);
        }

    void SetEnabled(bool enabled)
    {
        g_enabled.store(enabled, std::memory_order_relaxed);
    }

    void SetFullSoundCallers(bool full)
    {
        // Debug=0 drops every line the callers feed (POSE_V2_AUDIO_TOP and
        // the replay call's sounds are not essential): no walk at all.
        g_soundCallerDepth.store(full ? kCallerDepth : replay::log::Verbose() ? kShortCallerDepth : 0u,
            std::memory_order_relaxed);
    }

    void SetPlayRequestEvidence(bool on)
    {
        g_playRequestEvidence.store(on, std::memory_order_relaxed);
    }

    void SetPlayRequestMixProbe(bool on)
    {
        g_playRequestMixProbe.store(on, std::memory_order_relaxed);
    }

    void SetObserveEntityCreates(bool observe)
    {
        g_observeEntityCreates.store(observe, std::memory_order_relaxed);
    }

    std::uint64_t NowUs()
    {
        return replay::clock::NowUs(); // mod_clock.h (no 21-day overflow)
    }

    void CollectSounds(std::uint64_t fromUs, std::uint64_t toUs, std::vector<SoundCall>& out)
    {
        Collect(g_sounds, kSoundRing, g_nextSound, fromUs, toUs, out);
    }

    void CollectSpeech(std::uint64_t fromUs, std::uint64_t toUs, std::vector<SpeechCall>& out)
    {
        Collect(g_speech, kSpeechRing, g_nextSpeech, fromUs, toUs, out);
    }

    std::uint64_t TotalSounds()
    {
        return g_nextSound.load(std::memory_order_relaxed);
    }

    std::uint64_t TotalSpeech()
    {
        return g_nextSpeech.load(std::memory_order_relaxed);
    }

    std::uint32_t FinalizeHash(std::uint32_t hash)
    {
        hash += hash << 3;
        hash ^= hash >> 11;
        hash += hash << 15;
        return hash;
    }


    void Shutdown()
    {
        g_enabled.store(false, std::memory_order_relaxed);
        g_observeEntityCreates.store(false, std::memory_order_relaxed);
        SetPlayerAudioPeds({});
        if (g_playerAudioTarget)
        {
            MH_DisableHook(g_playerAudioTarget);
            MH_RemoveHook(g_playerAudioTarget);
            g_playerAudioTarget = nullptr;
        }
        SetPlayerMountAudioComponents({});
        SetSilencedFootAudios({});
        SetSilencedPuppetAudios({});
        SetSilencedPuppetWeaponAudios({});
        SetSilencedVehicleAudios({}, {});
        SetOwnPersistentCallers({});
        SetOwnPersistentKeys({});
        ClearFootMaterials();
        SetMovementTermState(false, false);
        g_movementDetailOn.store(false, std::memory_order_relaxed);
        if (g_movementVolumeTarget)
        {
            MH_DisableHook(g_movementVolumeTarget);
            MH_RemoveHook(g_movementVolumeTarget);
            g_movementVolumeTarget = nullptr;
        }
        if (g_mountAudioTarget)
        {
            MH_DisableHook(g_mountAudioTarget);
            MH_RemoveHook(g_mountAudioTarget);
            g_mountAudioTarget = nullptr;
        }
        // Disabled now, removed after the wait below (MH_RemoveHook frees the
        // trampoline a call in flight may still run).
        g_footEventRefuseOwn.store(false, std::memory_order_relaxed);
        if (g_footEventTarget)
            MH_DisableHook(g_footEventTarget);
        for (void** target : {&g_ctorTarget, &g_sayTarget, &g_fireTarget, &g_meleeImpactTarget, &g_coreTarget, &g_stopTarget,
                 &g_audibleTarget,
                 &g_footstepTarget, &g_movementUpdateTarget, &g_bodyImpactTarget, &g_explosionTarget, &g_pedAnimTarget,
                 &g_soundRequestTarget, &g_createAndPlayTarget, &g_persistentCreateTarget,
                 &g_stopAndForgetTarget, &g_byRefTarget, &g_byHashTarget})
        {
            if (*target)
            {
                MH_DisableHook(*target);
                MH_RemoveHook(*target);
                *target = nullptr;
            }
        }
        for (int wait = 0; wait < 200 && g_inFlight.load(std::memory_order_seq_cst) > 0; ++wait)
            Sleep(1);
        if (g_footEventTarget)
        {
            MH_RemoveHook(g_footEventTarget);
            g_footEventTarget = nullptr;
        }
        g_originalFootEvent.store(nullptr, std::memory_order_release);
        g_originalCtor.store(nullptr, std::memory_order_release);
        g_originalSay.store(nullptr, std::memory_order_release);
        g_originalFire.store(nullptr, std::memory_order_release);
        g_originalMeleeImpact.store(nullptr, std::memory_order_release);
        g_originalCore.store(nullptr, std::memory_order_release);
        g_originalStop.store(nullptr, std::memory_order_release);
        g_originalAudible.store(nullptr, std::memory_order_release);
        g_originalFootstep.store(nullptr, std::memory_order_release);
        g_originalExplosion.store(nullptr, std::memory_order_release);
        g_originalPedAnim.store(nullptr, std::memory_order_release);
        g_originalMovementUpdate.store(nullptr, std::memory_order_release);
        g_originalBodyImpact.store(nullptr, std::memory_order_release);
        g_originalSoundRequest.store(nullptr, std::memory_order_release);
        g_originalCreateAndPlay.store(nullptr, std::memory_order_release);
        g_originalPersistentCreate.store(nullptr, std::memory_order_release);
        g_originalStopAndForget.store(nullptr, std::memory_order_release);
        g_originalPlayerAudio.store(nullptr, std::memory_order_release);
        g_originalMountAudio.store(nullptr, std::memory_order_release);
    }
}
