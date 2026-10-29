#include <intrin.h>
#include "guarded_memory.h"
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
#include "sound_check_core.h" // KnownImageAudio

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
    namespace
    {
        // Game build 1491.50.
        constexpr std::uintptr_t kSoundParamsCtorRva = 0x487E34;
        constexpr std::uint8_t kSoundParamsCtorSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
            0x57, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0x57, 0xC0};
        constexpr std::uintptr_t kSpeechSayRva = 0x2F9FC84;
        constexpr std::uint8_t kSpeechSaySignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x60,
            0x48, 0x8B, 0xF9, 0x48, 0x8B, 0xDA};
        // Internal speech params (filled from the script params by the
        // native path): voice hash, partial context hash, variation.
        constexpr std::size_t kParamsVoice = 0x14C;
        constexpr std::size_t kParamsContext = 0x150;
        constexpr std::size_t kParamsVariation = 0x158;
        // The start offset in ms (PlaySpeechDirect in audio_observer.h has the
        // chain to the sound's init params).
        constexpr std::size_t kParamsStartOffset = 0x15C;

        // Pure getter: ped -> ped audio entity (reads the model table only).
        constexpr std::uintptr_t kPedAudioGetterRva = 0xCC81FC;
        constexpr std::uint8_t kPedAudioGetterSignature[] = {
            0x8B, 0x81, 0x9C, 0x00, 0x00, 0x00, 0x25, 0xFF, 0xFF, 0x01, 0x00};
        using PedAudioGetterFn = std::uint8_t*(__fastcall*)(void*);

        // The ped component table both getters read (0xCC56B4 at slot +0x30,
        // 0xCC81FC at +0xB0): entry = table + (index - base) * 0x148.
        constexpr std::uintptr_t kComponentGetterRva = 0xCC56B4;
        constexpr std::uint8_t kComponentGetterSignature[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x81, 0x9C, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9,
            0x4C, 0x8B, 0x15, 0x7E, 0x77, 0xD2, 0x02,  // mov r10, [table]
            0x25, 0xFF, 0xFF, 0x01, 0x00,
            0x2B, 0x05, 0x6B, 0x77, 0xD2, 0x02,        // sub eax, [index base]
            0x4C, 0x69, 0xC8, 0x48, 0x01, 0x00, 0x00}; // imul r9, rax, 148h
        constexpr std::uintptr_t kComponentTableRva = 0x39ECE48;
        constexpr std::uintptr_t kComponentIndexBaseRva = 0x39ECE40;

        constexpr std::uintptr_t kFireEnqueueRva = 0x2EE8D9C;
        constexpr std::uint8_t kFireEnqueueSignature[] = {
            0x48, 0x83, 0xEC, 0x28,             // sub rsp, 28h
            0x48, 0x63, 0x05, 0x49, 0x5C, 0xC7, 0x02, // movsxd rax, [count]
            0x48, 0x8B, 0xD1};                  // mov rdx, rcx
        using FireEnqueueFn = void(__fastcall*)(const void*);

        // Speech replay path (the ambient speech native, 0xF6A0CC).
        constexpr std::uintptr_t kSpeechParamsCtorRva = 0x2F91EA4;
        constexpr std::uint8_t kSpeechParamsCtorSignature[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x15};
        constexpr std::uintptr_t kSpeechParamsDtorRva = 0x143E98;
        constexpr std::uint8_t kSpeechParamsDtorSignature[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89, 0xA8, 0x00, 0x00, 0x00};
        using SpeechParamsFn = void(__fastcall*)(void*);

        // Melee impact audio entries.
        constexpr std::uintptr_t kMeleeImpactRva = 0x318A28C;
        constexpr std::uint8_t kMeleeImpactSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA, 0x48, 0x8B, 0xD9};
        using MeleeImpactFn = void(__fastcall*)(void*, void*);
        // The melee "definition" is the engine's action result (the melee move
        // from the action tables, vtable 0x3430988 built by 0xCD4574; +0x48 is
        // its name hash, e.g. AR_GRAPPLE_ATTACK 0x0C500859; +0x30 is the
        // material set the impact audio picks from). The engine finds one by
        // that hash with 0xCE0724(manager, hash): a binary search of the sorted
        // (hash, result) map at manager +0x30 (0x450210), no side effects.
        // TASK_MELEE's builder 0x1096C7C does exactly that at the site below
        // (mov rcx,[rip+manager]; mov edx,r15d; call), and so do the melee
        // clone tasks (0x167FA83); a melee event keeps only the name hash.
        constexpr std::uintptr_t kActionResultLookupSiteRva = 0x1096D40;
        constexpr std::size_t kActionResultManagerDisp = 3;
        constexpr std::size_t kActionResultLookupCall = 10;
        constexpr std::uint8_t kActionResultLookupSiteHead[] = {0x48, 0x8B, 0x0D};
        constexpr std::uint8_t kActionResultLookupSiteTail[] = {0x41, 0x8B, 0xD7, 0xE8};
        constexpr std::uintptr_t kActionResultLookupRva = 0xCE0724;
        constexpr std::uint8_t kActionResultLookupSignature[] = {
            0x48, 0x83, 0xEC, 0x28, 0x89, 0x54, 0x24, 0x38, 0x48, 0x83, 0xC1, 0x30, 0x48, 0x8D, 0x54,
            0x24, 0x38, 0xE8};
        using ActionResultLookupFn = void*(__fastcall*)(void*, std::uint32_t);

        // Deferred create-and-play (0x310E680): 8 arguments (audio entity,
        // u64 sound ref, owner entity, init params, then four byte flags on
        // the stack at entry +0x28..+0x40); returns bool.
        constexpr std::uintptr_t kSoundRequestRva = 0x310E680;
        constexpr std::uint8_t kSoundRequestSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x18,
            0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xC1, 0x48, 0x81, 0xEC, 0xA0};
        using SoundRequestFn = bool(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        // Release of the init params handle at +0x12C (no-op when -1).
        constexpr std::uintptr_t kSoundParamsReleaseRva = 0x253F0D0;
        constexpr std::uint8_t kSoundParamsReleaseSignature[] = {0x83, 0xCA, 0xFF, 0x66, 0x39, 0x11, 0x74, 0x12};
        using SoundParamsReleaseFn = void(__fastcall*)(void*);
        constexpr std::uint32_t kSoundRequestRing = 1024;
        // audEntity::CreateAndPlaySound core (0x2589A40): audio entity, u64
        // sound ref, init params; queues a one-shot. Reached through
        // 0x2588950 (ref) and 0x2588794 (hash).
        constexpr std::uintptr_t kCreateAndPlayRva = 0x2589A40;
        constexpr std::uint8_t kCreateAndPlaySignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x50, 0x10,
            0x57, 0x48, 0x81, 0xEC, 0x60};
        using CreateAndPlayFn = bool(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t);
        // Persistent sound creation used by collision code (0x2588FE8:
        // audio entity, sound descriptor with the ref at +0xC, audSound**
        // out, init params or null); the caller plays and updates the sound.
        constexpr std::uintptr_t kPersistentCreateRva = 0x2588FE8;
        constexpr std::uint8_t kPersistentCreateSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
            0x57, 0x48, 0x81, 0xEC, 0x60, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xE9, 0x49, 0x8B, 0xF9, 0x48,
            0x8D, 0x4C, 0x24, 0x20, 0x49, 0x8B, 0xF0, 0x48, 0x8B, 0xDA, 0xE8, 0x1F};
        using PersistentCreateFn = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        constexpr std::uintptr_t kStaticCollisionAudioRva = 0x5C10540;
        // The persistent create cores (audio entity, ref or hash, audSound**
        // out, init params or null); both start with `test r8, r8; je` (the
        // out slot).
        constexpr std::uintptr_t kPersistentByRefRva = 0x2589124;
        constexpr std::uint8_t kPersistentByRefSignature[] = {
            0x4D, 0x85, 0xC0, 0x0F, 0x84, 0x21, 0x01, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10};
        constexpr std::uintptr_t kPersistentByHashRva = 0x2589058;
        constexpr std::uint8_t kPersistentByHashSignature[] = {
            0x4D, 0x85, 0xC0, 0x0F, 0x84, 0xC1, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10};
        using PersistentByRefFn = void(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        using PersistentByHashFn = void(__fastcall*)(std::uint64_t, std::uint32_t, std::uint64_t, std::uint64_t);
        // Return address of the call to the by-ref core inside 0x2588FE8
        // (the collision entry, recorded by the collision track).
        constexpr std::uint32_t kCollisionCreateReturn = 0x2589030;
        // The peds' persistent sounds come this way too.
        constexpr std::uint32_t kEntitySoundRing = 1024;
        constexpr std::uint32_t kEntityTracked = 256;
        // StopAndForget (sound, continueUpdatingEntity); its body reads the
        // requested-settings pool (globals at +0x1D and +0x2B, rip-relative).
        constexpr std::uintptr_t kStopAndForgetRva = 0x2585C2C;
        constexpr std::uint8_t kStopAndForgetSignature[] = {
            0x4C, 0x8B, 0xC9, 0x44, 0x8A, 0xD2, 0x0F, 0xB7, 0x89, 0xD6, 0x00, 0x00, 0x00, 0x81, 0xF9,
            0xFF, 0xFF, 0x00, 0x00, 0x74, 0x27, 0x41, 0x0F, 0xB6};
        using StopAndForgetFn = void(__fastcall*)(std::uint64_t, std::uint8_t);
        constexpr std::uintptr_t kPrepareAndPlayRva = 0x257CCBC;
        constexpr std::uint8_t kPrepareAndPlaySignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18,
            0x48, 0x89, 0x78, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB, 0x41, 0x8B, 0xF9};
        using PrepareAndPlayFn = void(__fastcall*)(std::uint64_t, std::uint64_t, std::uint8_t, std::int32_t, std::uint8_t);
        constexpr std::uintptr_t kGetVariableRva = 0x2573738;
        constexpr std::uint8_t kGetVariableSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20,
            0x49, 0x8B, 0xF8, 0x8B, 0xF2, 0x48, 0x8B, 0xD9, 0xE8, 0x60, 0xFF, 0xFF, 0xFF};
        using GetVariableFn = bool(__fastcall*)(std::uint64_t, std::uint32_t, float*);
        using SetVariableFn = bool(__fastcall*)(std::uint64_t, const std::uint32_t*, float);
        // S38 (0x25735EC, slot 1 of the sound vtables) refuses a value that is
        // not finite and a hash its lookup 0x25736B4 does not find: the top
        // sound (0x25745A8 climbs +0xE2, the parent's slot) holds the block at
        // +0x9C (0xFFFF: none), 16 (hash, value) pairs and the count at +0x80,
        // in the sound pool (table at 0x53588D8, bucket +0xA0 times 0x2A860,
        // +0x2A848; slot stride at 0x53588B0). The lookup never adds: only a
        // variable block sound's Init (0x257A240) registers the variables it
        // exports. The reason of a refusal is read with these.
        constexpr std::uintptr_t kVariableFindRva = 0x25736B4;
        constexpr std::uint8_t kVariableFindSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0xE8, 0xE0, 0x0E, 0x00, 0x00,
            0x48, 0x85, 0xC0, 0x74, 0x57, 0x44, 0x0F, 0xB7, 0x80, 0x9C, 0x00, 0x00, 0x00, 0xB8, 0xFF, 0xFF, 0x00, 0x00, 0x66, 0x44,
            0x3B, 0xC0, 0x74, 0x44, 0x0F, 0xB6, 0x83, 0xA0, 0x00, 0x00, 0x00, 0x48, 0x69, 0xC8, 0x60, 0xA8, 0x02, 0x00, 0x48, 0x8B,
            0x05, 0xE3, 0x51, 0xDE, 0x02, 0x41, 0x0F, 0xB7, 0xD0, 0x0F, 0xAF, 0x15, 0xB0, 0x51, 0xDE, 0x02, 0x48, 0x03, 0x94, 0x01};
        constexpr std::uintptr_t kRootSoundRva = 0x25745A8;
        constexpr std::uint8_t kRootSoundSignature[] = {
            0x0F, 0xB7, 0x91, 0xE2, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xC1, 0x4C, 0x8B, 0x15, 0x1F, 0x43, 0xDE, 0x02, 0x44, 0x8B, 0x1D,
            0xF0, 0x42, 0xDE, 0x02};
        constexpr std::uintptr_t kSoundPoolTableRva = 0x53588D8;
        constexpr std::uintptr_t kSoundSlotStrideRva = 0x53588B0;
        // Requested-settings position setter (RS, Vec4*): mov eax,[rip+X]
        // then the fixed body; the displacement is masked.
        constexpr std::uintptr_t kSetPositionRva = 0x2536E48;
        constexpr std::uint8_t kSetPositionTail[] = {0x0F, 0x10, 0x02, 0x48, 0xFF, 0xC0, 0x48, 0x03, 0xC0, 0x0F};
        using SetPositionFn = void(__fastcall*)(std::uint64_t, const float*);
        // Public CreateAndPlaySound (ref): registers the audio entity when
        // needed, then calls the core.
        constexpr std::uintptr_t kCreateAndPlayPublicRva = 0x2588950;
        constexpr std::uint8_t kCreateAndPlayPublicSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20,
            0x83, 0x79, 0x14, 0x00, 0x49};

        constexpr std::uintptr_t kMeleeBodyImpactRva = 0x318EC48;
        constexpr std::uint8_t kMeleeBodyImpactSignature[] = {
            0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
            0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xB9, 0x48, 0x81, 0xEC, 0xC8};
        using MeleeBodyImpactFn = void(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t);

        using SoundParamsCtorFn = void*(__fastcall*)(void*);
        using SpeechSayFn = std::uint64_t(__fastcall*)(void*, void*);

        // Speech core (0x2FA1114): every line of every path is validated
        // and created here (speech entity, scope, internal params).
        constexpr std::uintptr_t kSpeechCoreRva = 0x2FA1114;
        constexpr std::uint8_t kSpeechCoreSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
            0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x81, 0xEC, 0x10, 0x02, 0x00, 0x00};
        using SpeechCoreFn = std::uint32_t(__fastcall*)(void*, void*, void*);

        // Direct line entry (0x2FA1280): copies the params and calls the
        // core, without the AI Say step (0x2F9FB30) that marks the request
        // as AI speech (+0x19D) and so enables the AI checks. The engine's
        // conversation code enters here, so a recorded line plays without AI
        // gating. The request scope is the one the Say wrapper builds and
        // releases (0x2F9FC84).
        constexpr std::uintptr_t kSpeechDirectRva = 0x2FA1280;
        constexpr std::uint8_t kSpeechDirectSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x81, 0xEC, 0x10, 0x02, 0x00, 0x00, 0x48, 0x8B,
            0xDA, 0x48, 0x8B, 0xF9, 0x49, 0x8B, 0xD0, 0x48, 0x8D, 0x4C, 0x24, 0x20, 0xE8, 0x54, 0x10,
            0xFF, 0xFF, 0x4C};
        constexpr std::uintptr_t kSpeechScopeCtorRva = 0x2F92204;
        constexpr std::uint8_t kSpeechScopeCtorSignature[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xBA, 0xE8, 0x03, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0xE8, 0x6D};
        constexpr std::uintptr_t kSpeechScopeReleaseRva = 0x2FBC728;
        constexpr std::uint8_t kSpeechScopeReleaseSignature[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x49, 0x28, 0x48, 0x85,
            0xC9, 0x74, 0x0D};
        constexpr std::uintptr_t kSpeechScopeDtorRva = 0x2595DA4;
        constexpr std::uint8_t kSpeechScopeDtorSignature[] = {
            0x48, 0xFF, 0x25, 0x7D, 0xC4, 0xA9, 0x03, 0x90, 0x48, 0x89, 0x5C, 0x24};
        // Line start (0x2FB5F0C): the AI step (0x2F9FB30) calls it on the
        // line the core attached to the scope (+0x28) unless that line is in
        // state 6 (+0x234), before the wrapper releases the scope.
        constexpr std::uintptr_t kSpeechLineStartRva = 0x2FB5F0C;
        constexpr std::uint8_t kSpeechLineStartSignature[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x81, 0xC1, 0x88, 0x01, 0x00,
            0x00, 0xE8, 0x97};
        using SpeechLineStartFn = std::uint32_t(__fastcall*)(void*);

        using SpeechDirectFn = std::uint32_t(__fastcall*)(void*, void*, void*);

        // Ambient voice setter behind SET_AMBIENT_VOICE_NAME (handler
        // 0xF63CD8 jumps here with the ped and the name): after hashing the
        // name it looks the voice object up (store, hash, type 0x22), asks
        // the speech entity for its voice holder (vtable +0x128) and applies
        // the voice (holder, voice, 1, 1, callback). The replay repeats the
        // steps after the hash with the recorded voice hash; every address
        // is read from these verified bytes.
        constexpr std::uintptr_t kVoiceSetterRva = 0xF73D80;
        constexpr std::uint8_t kVoiceSetterSignature[] = {
            0x48, 0x85, 0xC9, 0x0F, 0x84, 0xA5, 0x00, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48,
            0x83, 0xEC, 0x40, 0x48, 0x8B, 0xFA, 0xE8, 0x19, 0x19, 0xD5, 0xFF, 0x48, 0x85, 0xC0, 0x0F, 0x84,
            0x80, 0x00, 0x00, 0x00, 0xF6, 0x80, 0x18, 0x02, 0x00, 0x00, 0x02, 0x74, 0x10, 0x48, 0x8B, 0x80,
            0x98, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x98, 0x38, 0x44, 0x00, 0x00, 0xEB, 0x02, 0x33, 0xDB, 0x48,
            0x85, 0xDB, 0x74, 0x60, 0x33, 0xD2, 0x48, 0x8B, 0xCF, 0xE8, 0x46, 0x6D, 0x68, 0x01, 0x41, 0xB0,
            0x22, 0x48, 0x8D, 0x0D, 0x78, 0x36, 0xC1, 0x04, 0x8B, 0xD0, 0xE8, 0xA5, 0x77, 0x5B, 0x01, 0x48,
            0x8B, 0xF8, 0x48, 0x85, 0xC0, 0x74, 0x3D, 0x48, 0x8B, 0x13, 0x48, 0x8B, 0xCB, 0xFF, 0x92, 0x28,
            0x01, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x2C, 0x83, 0x64, 0x24, 0x38, 0x00, 0x48, 0x8D, 0x0D,
            0xAC, 0x12, 0x4C, 0x02, 0x48, 0x89, 0x4C, 0x24, 0x30, 0x41, 0xB1, 0x01, 0x48, 0x8D, 0x4C, 0x24,
            0x30, 0x45, 0x8A, 0xC1, 0x48, 0x89, 0x4C, 0x24, 0x20, 0x48, 0x8B, 0xD7, 0x48, 0x8B, 0xC8, 0xE8,
            0xA0, 0x2E, 0x02, 0x02};
        constexpr std::size_t kVoiceStoreDisp = 0x54;    // lea rcx, [store]
        constexpr std::size_t kVoiceLookupCall = 0x5A;   // call lookup
        constexpr std::size_t kVoiceCallbackDisp = 0x80; // lea rcx, [callback]
        constexpr std::size_t kVoiceApplyCall = 0x9F;    // call apply
        using VoiceLookupFn = void*(__fastcall*)(void*, std::uint32_t, std::uint8_t);
        using VoiceHolderFn = void*(__fastcall*)(void*);
        using VoiceApplyFn = void(__fastcall*)(void*, void*, std::uint8_t, std::uint8_t, void*);
        using SpeechScopeFn = void(__fastcall*)(void*);
        constexpr std::uint32_t kCoreRing = 512;

        // Line stop (0x2FBCCD4, line + reason): every path that ends a line
        // before or after it plays goes through here (50 call sites).
        constexpr std::uintptr_t kSpeechLineStopRva = 0x2FBCCD4;
        constexpr std::uint8_t kSpeechLineStopSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
            0x57, 0x48, 0x83, 0xEC, 0x20, 0x83, 0xB9, 0x34};
        using SpeechLineStopFn = void(__fastcall*)(void*, std::uint32_t);
        constexpr std::uint32_t kStopRing = 256;

        // Audible start of a line (0x2FB99E8, called by the line update
        // 0x2FB4C24 at 0x2FB4C8D and 0x2FB4CC5; plays the line data (vtable
        // +0x58) and moves the line to state 5).
        constexpr std::uintptr_t kSpeechLineAudibleRva = 0x2FB99E8;
        constexpr std::uint8_t kSpeechLineAudibleSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18,
            0x48, 0x89, 0x78, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF9};
        using SpeechLineAudibleFn = void(__fastcall*)(void*);
        constexpr std::uint32_t kStartRing = 256;

        // The two calls of the voice setter the replay repeats (targets read
        // from the setter's own bytes, then checked against these RVAs and
        // prologues): the typed store lookup 0x252B584 (store, hash, type;
        // returns the object only when [obj+0] == type) and the apply
        // 0x2F96CC4 (holder, voice, 1, 1, callback; writes holder +0x30).
        constexpr std::uintptr_t kVoiceLookupRva = 0x252B584;
        constexpr std::uint8_t kVoiceLookupSignature[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x41, 0x0F, 0xB6, 0xD8, 0x45, 0x33, 0xC9, 0x44, 0x8B, 0xC3};
        constexpr std::uintptr_t kVoiceApplyRva = 0x2F96CC4;
        constexpr std::uint8_t kVoiceApplySignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48,
            0x83, 0xEC, 0x50};

        // Footstep entry (0x317A340): (footAudio, id, arg3, foot, arg5, flag,
        // arg7, arg8); the body reads flag and arg7 from the stack. Called by
        // the foot-down handler 0x8C2B64 with holder +0x1560 once holder
        // +0x1584 is set.
        constexpr std::uintptr_t kFootstepRva = 0x317A340;
        constexpr std::uint8_t kFootstepSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18,
            0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC,
            0x40, 0x45, 0x8B, 0xE8, 0x45, 0x8A, 0xF9, 0x45, 0x33, 0xC0};
        using FootstepFn = void(__fastcall*)(void*, std::int32_t, std::uint32_t, std::uint8_t,
            std::uint8_t, std::uint8_t, std::uint32_t, std::uint8_t);
        constexpr std::uint32_t kFootstepRing = 1024;
        // Ped movement audio update (0x3193D14): one argument, the ped's foot
        // audio (holder +0x1560, [+0x18] = ped). Plays the queued footsteps
        // (0x317A1D0) and the movement sounds (0x3193F20: scuffs, hooves) from
        // the ped's own animation, which a puppet regenerates by itself.
        constexpr std::uintptr_t kMovementUpdateRva = 0x3193D14;
        constexpr std::uint8_t kMovementUpdateSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18,
            0x57, 0x48, 0x83, 0xEC, 0x50};
        using MovementUpdateFn = void(__fastcall*)(void*);
        constexpr std::uintptr_t kFootAudioOffset = 0x1560;
        // The second queue's entry 0x317A1D0 and its sites (the signatures
        // live in foot_event_core.h, where the tests check the calls' targets).
        using FootEventFn = void(__fastcall*)(void*, std::int32_t, std::uint8_t, std::uint8_t, std::uint8_t);

        // Explosion audio.
        constexpr std::uintptr_t kExplosionEnqueueRva = 0x2EF0010;
        constexpr std::uint8_t kExplosionEnqueueSignature[] = {
            0x48, 0x83, 0xEC, 0x28, 0x48, 0x83, 0x7A, 0x08, 0x00, 0x4C, 0x8B, 0xC1};
        using ExplosionEnqueueFn = void(__fastcall*)(void* manager, const void* request);
        constexpr std::uintptr_t kExplosionTagRefRva = 0x2EF10D8; // (u32* out, tag) -> out
        constexpr std::uint8_t kExplosionTagRefSignature[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x83, 0xFA, 0x24};
        using ExplosionTagRefFn = std::uint32_t*(__fastcall*)(std::uint32_t* out, std::int32_t tag);
        constexpr std::uintptr_t kExplosionPlayRva = 0x2EF01EC;
        constexpr std::uint8_t kExplosionPlaySignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x18, 0x4C,
            0x89, 0x70, 0x20, 0x55, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x60, 0x83, 0xB9, 0x00, 0x04, 0x00};
        // (manager, ref, pos*, underwater, b31, interior, b32): the native's call.
        using ExplosionPlayFn = void(__fastcall*)(void* manager, std::uint32_t ref, const float* position,
            std::uint8_t underwater, std::uint8_t b31, std::uint32_t interior, std::uint8_t b32);
        constexpr std::uintptr_t kExplosionManagerRva = 0x5B9FC30;
        constexpr std::uint32_t kExplosionRing = 256;
        constexpr std::uintptr_t kPedAnimHandlerRva = 0x3193CAC;
        constexpr std::uint8_t kPedAnimHandlerSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
            0x8D, 0x59, 0x78, 0x48, 0x8B, 0xF9, 0x48, 0x63};
        using PedAnimHandlerFn = void(__fastcall*)(void* audio);
        // AddAnimEvent (audio, event*): slot +0xB8 of the three ped audio
        // vtables; returns when [+0x14] == 0 or the count [+0xD8] is 4 or
        // 0x2C2A9F8 finds the same {extra, hash}; registers the entity
        // (0x2589970) when [+0x10] == 0xFFFF; copies the 0x18 bytes to +0x78 +
        // count * 0x18. The prologue plus the first compare (34 bytes) is
        // unique in .text.
        constexpr std::uintptr_t kAddAnimEventRva = 0x2C2D934;
        constexpr std::uint8_t kAddAnimEventSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x83,
            0x79, 0x14, 0x00, 0x48, 0x8B, 0xF2, 0x48, 0x8B, 0xD9, 0x74, 0x53, 0x83, 0xB9, 0xD8, 0x00, 0x00,
            0x00, 0x04};
        using AddAnimEventFn = void(__fastcall*)(void* audio, const void* event);
        constexpr std::uintptr_t kPedAudioVtableRvas[] = {0x3839538, 0x3839630, 0x3839728};
        constexpr std::uint32_t kAnimRing = 1024;
        constexpr std::uint32_t kSoundRing = 16384;
        constexpr std::uint32_t kSpeechRing = 1024;

        // The rings' slots and protocol (event_ring.h): writers claim and
        // publish, readers stop at a ticket not yet published.
        template <typename T>
        using Slot = replay::ring::Slot<T>;

        Slot<SoundCall> g_sounds[kSoundRing];
        std::atomic<std::uint64_t> g_nextSound{0};
        Slot<SpeechCall> g_speech[kSpeechRing];
        std::atomic<std::uint64_t> g_nextSpeech{0};
        Slot<SpeechCoreCall> g_core[kCoreRing];
        std::atomic<std::uint64_t> g_nextCore{0};
        std::atomic<SpeechCoreFn> g_originalCore{nullptr};
        void* g_coreTarget = nullptr;
        Slot<SpeechLineStop> g_stops[kStopRing];
        std::atomic<std::uint64_t> g_nextStop{0};
        // The puppets' speech entities and their own core calls
        // (puppet_audio_rule.h RefusePuppetOwnSpeech).
        constexpr std::size_t kMaxPuppetSpeech = 256;
        std::atomic<std::uint64_t> g_puppetSpeech[kMaxPuppetSpeech] = {};
        std::atomic<std::uint32_t> g_puppetSpeechOwner[kMaxPuppetSpeech] = {};
        std::atomic<std::uint32_t> g_puppetSpeechCount{0};
        std::atomic<bool> g_puppetOwnSpeechBlock{true};
        using SpeechScopeReleaseFn = void(__fastcall*)(void*);
        std::atomic<SpeechScopeReleaseFn> g_ownSpeechRelease{nullptr};
        const char* g_ownSpeechError = "not_installed";
        std::atomic<std::uint64_t> g_puppetOwnSpeechSeen{0};
        std::atomic<std::uint64_t> g_puppetOwnSpeechRefused{0};
        // Core calls on other speech entities while puppets are listed (the
        // live world talking behind the replay: its lines take wave slots).
        std::atomic<std::uint64_t> g_worldSpeechDuringPlayback{0};
        std::atomic<std::uint64_t> g_worldSpeechQueuedDuringPlayback{0};
        constexpr std::uint32_t kOwnSpeechRing = 512;
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
        void* g_stopTarget = nullptr;
        Slot<SpeechLineStart> g_starts[kStartRing];
        std::atomic<std::uint64_t> g_nextStart{0};
        std::atomic<SpeechLineAudibleFn> g_originalAudible{nullptr};
        void* g_audibleTarget = nullptr;
        Slot<ExplosionAudioCall> g_explosions[kExplosionRing];
        std::atomic<std::uint64_t> g_nextExplosion{0};
        std::atomic<ExplosionEnqueueFn> g_originalExplosion{nullptr};
        void* g_explosionTarget = nullptr;
        thread_local bool t_playingExplosion = false;
        // What the enqueuer did with our playback request (-1 not reached,
        // 0 not stored, 1 stored), read by the detour.
        thread_local int t_playQueued = -1;
        thread_local std::int32_t t_playQueueBefore = -1;
        Slot<AnimAudioBatch> g_animBatches[kAnimRing];
        std::atomic<std::uint64_t> g_nextAnimBatch{0};
        std::atomic<PedAnimHandlerFn> g_originalPedAnim{nullptr};
        void* g_pedAnimTarget = nullptr;
        // Counters of what the engine creates inside one ped anim handler call.
        thread_local int t_animScopeDepth = 0;
        thread_local std::uint32_t t_animSounds = 0;
        thread_local std::uint32_t t_animFootsteps = 0;
        thread_local std::uint32_t t_animSpeech = 0;
        Slot<FootstepCall> g_footsteps[kFootstepRing];
        std::atomic<std::uint64_t> g_nextFootstep{0};
        std::atomic<FootstepFn> g_originalFootstep{nullptr};
        void* g_footstepTarget = nullptr;
        std::atomic<MovementUpdateFn> g_originalMovementUpdate{nullptr};
        void* g_movementUpdateTarget = nullptr;

        constexpr std::uint32_t kMeleeRing = 256;
        Slot<MeleeCall> g_melee[kMeleeRing];
        std::atomic<std::uint64_t> g_nextMelee{0};
        constexpr std::uint32_t kImpactRing = 256;
        Slot<ImpactCall> g_impacts[kImpactRing];
        std::atomic<std::uint64_t> g_nextImpact{0};
        std::atomic<MeleeBodyImpactFn> g_originalBodyImpact{nullptr};
        void* g_bodyImpactTarget = nullptr;
        Slot<SoundRequest> g_soundRequests[kSoundRequestRing];
        std::atomic<std::uint64_t> g_nextSoundRequest{0};
        std::atomic<SoundRequestFn> g_originalSoundRequest{nullptr};
        void* g_soundRequestTarget = nullptr;
        Slot<SoundRequest> g_playRequests[kSoundRequestRing];
        std::atomic<std::uint64_t> g_nextPlayRequest{0};
        std::atomic<std::uint64_t> g_refusedPlayRequests{0}; // create-and-play calls the engine refused (not recorded)
        // Foot audios whose own foot-downs are not played (the replay's
        // puppets and the hidden player while a clip plays; the replay plays
        // the recorded steps through the original). Written by the script
        // thread, read by the detour on any thread; a stale entry for a moment
        // is harmless.
        constexpr std::size_t kMaxSilencedFeet = 256;
        std::atomic<std::uint64_t> g_silencedFeet[kMaxSilencedFeet] = {};
        std::atomic<std::uint32_t> g_silencedFeetCount{0};
        std::atomic<std::uint64_t> g_silencedFootsteps{0};

        bool IsSilencedFoot(const void* footAudio)
        {
            const auto value = reinterpret_cast<std::uint64_t>(footAudio);
            const std::uint32_t count = g_silencedFeetCount.load(std::memory_order_acquire);
            for (std::uint32_t n = 0; n < count && n < kMaxSilencedFeet; ++n)
                if (g_silencedFeet[n].load(std::memory_order_relaxed) == value)
                    return true;
            return false;
        }
        // puppet_audio_rule.h: the ped audio holders of the puppets, the
        // hidden player and his mount while a clip plays; a create-and-play on
        // them that the replay did not make is refused. Written by the script
        // thread, read by the detour on any thread.
        constexpr std::size_t kMaxSilencedPuppetAudios = 256;
        std::atomic<std::uint64_t> g_silencedPuppetAudio[kMaxSilencedPuppetAudios] = {};
        std::atomic<std::uint32_t> g_silencedPuppetAudioCount{0};
        std::atomic<std::uint64_t> g_refusedPuppetOneShots{0};
        constexpr std::size_t kRefusedOneShotSamples = 16;
        std::atomic<std::uint64_t> g_refusedOneShotRef[kRefusedOneShotSamples] = {};
        std::atomic<std::uint32_t> g_refusedOneShotCaller[kRefusedOneShotSamples] = {};
        std::atomic<std::uint64_t> g_refusedOneShotBase{0}; // samples count from here (BeginRefusedPuppetOneShots)
        thread_local std::uint32_t t_replayEmitDepth = 0;

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
        // puppet_audio_rule.h: the weapon puppets' CWeapons (one-shots on
        // [CWeapon, CWeapon + kWeaponOneShotSpan) are refused like the
        // holders'), and the call sites of the clip's ESND ped audio sounds
        // (a puppet's own persistent create from one of them is refused).
        // Written by the script thread, read by the detours on any thread.
        constexpr std::size_t kMaxSilencedPuppetWeapons = 256;
        std::atomic<std::uint64_t> g_silencedPuppetWeapon[kMaxSilencedPuppetWeapons] = {};
        std::atomic<std::uint32_t> g_silencedPuppetWeaponCount{0};
        std::atomic<std::uint64_t> g_refusedPuppetWeaponOneShots{0};
        constexpr std::size_t kMaxOwnPersistentCallers = 128;
        std::atomic<std::uint32_t> g_ownPersistentCaller[kMaxOwnPersistentCallers] = {};
        std::atomic<std::uint32_t> g_ownPersistentCallerCount{0};
        std::atomic<bool> g_refuseOwnPersistent{true};
        std::atomic<std::uint64_t> g_refusedOwnPersistent{0};
        std::atomic<std::uint64_t> g_keptOwnPersistent{0};
        std::atomic<std::uint64_t> g_refusedOwnPersistentBase{0};
        std::atomic<std::uint64_t> g_keptOwnPersistentBase{0};
        constexpr std::size_t kOwnPersistentSamples = 12;
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
        constexpr std::size_t kBlockedRing = 8192;
        Slot<BlockedPuppetCreate> g_blocked[kBlockedRing];
        std::atomic<std::uint64_t> g_nextBlocked{0};
        // What the ports judge against (sound_owner_table.h PacketLives,
        // published by the generic player at its start) and the playhead;
        // SoundGenericBlock=all refuses everything.
        std::atomic<const replay::soundowner::PacketLives*> g_blockLives{nullptr};
        std::atomic<bool> g_blockAll{false};
        std::atomic<std::uint32_t> g_blockPlayheadMs{0};
        // SoundStepSetGate (puppet_audio_rule.h RefuseUnmadeReplayedSet).
        std::atomic<bool> g_stepSetGate{true};
        std::atomic<std::uint64_t> g_refusedUnmadeSets{0};
        // puppet_audio_rule.h RefuseUnrecordedOwnPersistent: ini
        // PuppetUnrecordedPersistent (default 1), the count and the first
        // refusals (owner, ref, call site) for the evidence line.
        std::atomic<bool> g_refuseUnrecordedPersistent{true};
        std::atomic<std::uint64_t> g_unrecordedRefused{0};
        std::atomic<std::uint64_t> g_unrecordedKeptByEsnd{0};
        constexpr std::size_t kUnrecordedSamples = 8;
        std::atomic<std::uint32_t> g_unrecordedOwner[kUnrecordedSamples] = {};
        std::atomic<std::uint64_t> g_unrecordedRef[kUnrecordedSamples] = {};
        std::atomic<std::uint32_t> g_unrecordedSite[kUnrecordedSamples] = {};
        std::atomic<std::uint32_t> g_unrecordedSampleCount{0};
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
        // puppet_audio_rule.h OwnPersistentCarried: the clip's ESND pedAudio
        // sounds as (entity, call site, entry) keys, sorted and published once
        // per playback; the switch (ini PuppetPersistentByEntry).
        constexpr std::size_t kMaxOwnPersistentKeys = 4096;
        std::atomic<std::uint64_t> g_ownPersistentKey[kMaxOwnPersistentKeys] = {};
        std::atomic<std::uint32_t> g_ownPersistentKeyCount{0};
        std::atomic<bool> g_persistentByEntry{true};
        // The samples' holder entity and the two game frames above the call
        // site (evidence: which puppet, which engine path).
        std::atomic<std::uint32_t> g_refusedPersistentOwner[kOwnPersistentSamples] = {};
        std::atomic<std::uint32_t> g_refusedPersistentFrame[kOwnPersistentSamples][2] = {};
        std::atomic<std::uint32_t> g_keptPersistentOwner[kOwnPersistentSamples] = {};
        std::atomic<std::uint32_t> g_keptPersistentFrame[kOwnPersistentSamples][2] = {};

        // puppetaudio::OwnPersistentCarried on the published lists, read in
        // place (SortedKeyCarried's binary search on the atomics, no copy of
        // the keys on the detour's stack).
        bool OwnPersistentCreateCarried(std::uint32_t owner, std::uint32_t caller, std::uint64_t soundRef, bool byHash)
        {
            if (!caller)
                return false;
            if (!g_persistentByEntry.load(std::memory_order_relaxed) || owner == replay::puppetaudio::kNoOwner)
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
        std::atomic<PersistentCreateFn> g_originalPersistentCreate{nullptr};
        void* g_persistentCreateTarget = nullptr;
        std::atomic<PersistentByRefFn> g_originalByRef{nullptr};
        void* g_byRefTarget = nullptr;
        std::atomic<PersistentByHashFn> g_originalByHash{nullptr};
        void* g_byHashTarget = nullptr;
        Slot<EntitySoundCreate> g_entitySounds[kEntitySoundRing];
        std::atomic<std::uint64_t> g_nextEntitySound{0};
        // Entity sounds created while recording: their stop is recorded too.
        std::atomic<std::uint64_t> g_entityTracked[kEntityTracked] = {};
        std::atomic<std::uint32_t> g_nextEntityTracked{0};
        std::atomic<StopAndForgetFn> g_originalStopAndForget{nullptr};
        void* g_stopAndForgetTarget = nullptr;
        // Collision sounds created while recording, so the stop detour
        // records only those (547 callers stop every kind of sound).
        constexpr std::uint32_t kTrackedSounds = 64;
        std::atomic<std::uint64_t> g_trackedSounds[kTrackedSounds] = {};
        std::atomic<std::uint32_t> g_nextTracked{0};
        constexpr std::uint32_t kStopRing2 = 256;
        Slot<SoundStop> g_soundStops[kStopRing2];
        std::atomic<std::uint64_t> g_nextSoundStop{0};
        std::atomic<MeleeImpactFn> g_originalMeleeImpact{nullptr};
        void* g_meleeImpactTarget = nullptr;

        constexpr std::uint32_t kFireRing = 256;
        Slot<FireCall> g_fires[kFireRing];
        std::atomic<std::uint64_t> g_nextFire{0};
        std::atomic<FireEnqueueFn> g_originalFire{nullptr};
        void* g_fireTarget = nullptr;
        FireEnqueueFn g_fireDirect = nullptr;

        std::atomic<SoundParamsCtorFn> g_originalCtor{nullptr};
        std::atomic<SpeechSayFn> g_originalSay{nullptr};
        std::atomic<bool> g_enabled{false};
        constexpr std::uint32_t kShortCallerDepth = 3; // callers[0..2], what POSE_V2_AUDIO_TOP reads
        std::atomic<std::uint32_t> g_soundCallerDepth{kShortCallerDepth};
        // horse_audio_evidence.h: the create-and-play ring also records while
        // a clip plays (evidence only; nothing reads it but the horse audio
        // evidence, which has its own cursor).
        std::atomic<bool> g_playRequestEvidence{false};
        std::atomic<bool> g_playRequestMixProbe{false}; // the mix probe reads the ring
        // Playback watch of the entity creates (the puppets' own).
        std::atomic<bool> g_observeEntityCreates{false};
        std::atomic<std::int32_t> g_inFlight{0};
        void* g_ctorTarget = nullptr;
        void* g_sayTarget = nullptr;
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
        LARGE_INTEGER g_frequency{};

        // Depth of footstep/melee action calls on this thread: sounds made
        // inside them are replayed by those systems, not as generic sounds.
        thread_local std::uint32_t t_replayedSystemDepth = 0;
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
            for (std::uint32_t depth = 0; depth < kCallerDepth; ++depth)
                slot.value.callers[depth] = depth < captured ? ToRva(frames[depth]) : 0;
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
            // (0x2FA1163: scope released, 0x3F, no line).
            std::uint32_t puppetOwner = 0;
            if (t_replayEmitDepth == 0 && PuppetSpeechOwner(reinterpret_cast<std::uint64_t>(speechEntity), puppetOwner))
            {
                const SpeechScopeReleaseFn release = g_ownSpeechRelease.load(std::memory_order_acquire);
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
                    release(scope);
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
                // params +0xC0..+0xCF, copied by the data init at 0x2FA9327
                // before the core returns).
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
                    // callback never writes (0x2BC0AC0: and word [rbp-0x18]).
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

        bool WriteFootMix(std::uint8_t* footAudio, std::uint8_t foot, const footmaterial::FootMix& mix)
        {
            if (!footAudio || foot >= footmaterial::kFeet || !mix.known)
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
            const std::uint64_t component = foot < footmaterial::kFeet ? LocomotionComponent(footAudio) : 0;
            float current = 0.0f;
            if (!component || !GuardedRead(&current, reinterpret_cast<const void*>(component + footmaterial::ImpactOffset(foot)), sizeof(current)))
                return false;
            if (current == impact)
                return false;
            return replay::mem::Put(component + footmaterial::ImpactOffset(foot), &impact, sizeof(impact));
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
            // (+0x24, 0x8C2B64): a step it drops made no sound in the game and
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
            // read it (the quadruped's hit pattern 0x317AAD0 wrote it right
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
        std::atomic<bool> g_footMaterialReplay{true};
        std::atomic<std::uint64_t> g_footMaterialDetourWrites{0};
        std::atomic<std::uint64_t> g_footMaterialSampled{0};
        std::atomic<std::uint64_t> g_footWetnessDetourWrites{0};
        std::atomic<bool> g_footWetnessReplay{true};             // ini FootWetnessReplay
        std::atomic<std::uint64_t> g_footMixDetourWrites{0};
        std::atomic<bool> g_footMixReplay{true};                 // ini FootMixReplay
        std::atomic<bool> g_footBaseReplay{true};                // ini FootBaseVolumeReplay
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
            return foot < footmaterial::kFeet &&
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
                // (0x318D338), left by the previous foot update.
                float wetness = -1.0f;
                if (feet[n] < footmaterial::kFeet &&
                    GuardedRead(&wetness, footAudio + footmaterial::WetnessOffset(feet[n]), sizeof(wetness)) &&
                    (!(wetness >= 0.0f) || wetness > 16.0f))
                    wetness = -1.0f;
                sample.material.wetness = wetness;
                // foot_material.h kLayerBase: the ground layer weights this
                // step reads (0x318D297), written by the foot update's
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
                for (std::uint8_t foot = 0; foot < footmaterial::kFeet && g_footMaterialReplay.load(std::memory_order_relaxed); ++foot)
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
                // 0x31A67E8 set the replay world's).
                if (g_footWetnessReplay.load(std::memory_order_relaxed))
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
                // steps about to play (the puppet's own [vtable +0x80] 0x319CBF0
                // just wrote its own).
                if (g_footLayerReplay.load(std::memory_order_relaxed))
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
                // 0x317AAD0 that writes the slot).
                if (g_footMixReplay.load(std::memory_order_relaxed))
                {
                    for (std::uint8_t foot = 0; foot < footmaterial::kFeet; ++foot)
                    {
                        footmaterial::FootMix current;
                        footmaterial::FootMix desired = mix[foot];
                        float puppetBase = 0.0f;
                        if (desired.known && ReadFootBase(footAudio, puppetBase))
                            desired.volume = footmaterial::SlotVolumeWithBase(desired.volume, drive[foot], puppetBase,
                                g_footBaseReplay.load(std::memory_order_relaxed));
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

        void __fastcall MovementUpdateDetour(void* footAudio)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.audio_movement");
            InFlight inFlight;
            // Before the queued steps play (0x3193F20).
            if (footAudio)
            {
                auto* foot = static_cast<std::uint8_t*>(footAudio);
                std::uint8_t feet[footmaterial::kQueueMax] = {};
                const std::uint32_t queued = QueuedFeet(foot, feet);
                // The secondary steps the replay queued on a puppet (second
                // queue +0xB4, count +0xD4) read the same slots when 0x3193F20
                // plays them.
                std::int32_t secondary = 0;
                if (!queued && g_silencedFeetCount.load(std::memory_order_relaxed) != 0)
                    GuardedRead(&secondary, foot + footevent::kQueueCountOffset, sizeof(secondary));
                if (queued || secondary > 0)
                {
                    if (IsSilencedFoot(footAudio))
                    {
                        if (g_footMaterialReplay.load(std::memory_order_relaxed) ||
                            g_footWetnessReplay.load(std::memory_order_relaxed) ||
                            g_footMixReplay.load(std::memory_order_relaxed) ||
                            g_footLayerReplay.load(std::memory_order_relaxed) ||
                            g_footImpactReplay.load(std::memory_order_relaxed))
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

        // foot_event_core.h: the second queue's entry 0x317A1D0, detoured. The
        // generator (0x8ED457) and the drain (0x3193D96) are told apart by the
        // return address; an entry the engine queued is read back at the
        // index the count had before the call (the store 0x317A31A is the
        // count's only increment). A puppet's own event is refused only while
        // a clip with FSEV plays (the replay queues the recorded ones through
        // the original, PlayFootEvent). No native, no write.
        constexpr std::uint32_t kFootEventRing = 2048;
        Slot<FootEventCall> g_footEvents[kFootEventRing];
        std::atomic<std::uint64_t> g_nextFootEvent{0};
        std::atomic<FootEventFn> g_originalFootEvent{nullptr};
        void* g_footEventTarget = nullptr;
        const char* g_footEventError = "not_installed";
        std::atomic<bool> g_footEventRefuseOwn{false};
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
            call.origin = static_cast<std::uint8_t>(footevent::OriginOf(ToRva(_ReturnAddress())));
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
                    // update, right before 0x3193F20).
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
                // 0x2C3A7CC on line +0xC0: r = [+0xD0]; o = [r+0x20]; parent =
                // [o+0x18] when [o+0x10] == r.
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

        // Audio categories (init params +0xD8) seen in live requests of this
        // process. Recorded params keep the category pointer the game gave,
        // but a pointer from a clip is only trusted when this process has seen
        // the same category object: categories live as long as the audio
        // engine, so such a pointer is valid here.
        constexpr std::size_t kSoundCategoryOffset = 0xD8;
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

        // Whether packets mode refuses this create (and notes it).
        bool BlockedInPackets(std::uint8_t port, std::uint64_t audio, std::uint64_t soundRef, std::uint32_t caller)
        {
            if (!g_blockPuppetSounds.load(std::memory_order_relaxed) ||
                g_silencedPuppetAudioCount.load(std::memory_order_relaxed) == 0)
                return false;
            std::uint32_t owner = replay::puppetaudio::kNoOwner;
            if (!replay::puppetaudio::BlockPuppetCreate(true, SilencedPuppetAudioOwner(audio, owner), t_replayEmitDepth > 0,
                    t_replayedSystemDepth > 0, caller))
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
                if (replay::puppetaudio::RefuseUnmadeReplayedSet(g_stepSetGate.load(std::memory_order_relaxed), onPuppet,
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
                const std::uint64_t collision =
                    reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr)) + kStaticCollisionAudioRva;
                if (request.sound && audio == collision)
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
            if (caller == kCollisionCreateReturn)
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

        void ExpectQueuedCreate(std::uint64_t audio, std::uint64_t soundRef)
        {
            const std::size_t n = g_expectedNext.fetch_add(1, std::memory_order_relaxed) % kExpectedQueued;
            g_expectedAudio[n].store(0, std::memory_order_release);
            g_expectedRef[n].store(soundRef, std::memory_order_relaxed);
            g_expectedUntilUs[n].store(QpcUs() + 2000000ull, std::memory_order_relaxed);
            g_expectedAudio[n].store(audio, std::memory_order_release);
        }

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

        // Mode c's tickets. Each request through the queue holds one entry
        // (audio, ref, its window); the creator (census, S01/S02) gives the
        // first sound it makes for that (audio, ref) to the oldest waiting
        // entry and writes the birth to a ring the player drains by ticket.
        // Script thread writes the entries, any thread takes them.
        constexpr std::size_t kQueueTickets = 64;
        std::atomic<std::uint64_t> g_ticketAudio[kQueueTickets] = {};
        std::atomic<std::uint64_t> g_ticketRef[kQueueTickets] = {};
        std::atomic<std::uint64_t> g_ticketUntilUs[kQueueTickets] = {};
        std::atomic<std::uint64_t> g_ticketId[kQueueTickets] = {};
        std::atomic<std::uint64_t> g_ticketNext{0};
        std::atomic<std::uint32_t> g_ticketsWaiting{0};
        using QueueBirthSlot = replay::ring::Slot<QueueBirth>;
        constexpr std::size_t kQueueBirthRing = 256;
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

        // puppet_audio_rule.h: a puppet's own persistent sound without tracker
        // from a call site the clip's ESND track carries. Refused as the core
        // refuses an unregistered audio entity (0x258922E by ref, 0x2589101 by
        // hash: *out = 0, nothing created). Carried means the same entity,
        // call site and sound entry in the clip (OwnPersistentCreateCarried;
        // the call site alone with PuppetPersistentByEntry=0 or on the hidden
        // player and his mount).
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
                        t_replayEmitDepth > 0, t_replayedSystemDepth > 0, site))
                    return false;
                // Only what the recording plays (or a proved double) is refused;
                // the rest is left to the engine and counted.
                bool refuse = true;
                replay::soundowner::LifeVerdict verdict = JudgeOwn(blockedOwner, soundRef, refuse);
                // A persistent sound by ref the owner never made anywhere in the
                // clip and the ESND track does not carry: refused too.
                if (!refuse && !byHash && g_refuseUnrecordedPersistent.load(std::memory_order_relaxed))
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
            if (!replay::puppetaudio::LegacyRefuseOwnPersistent(caller, true, t_replayEmitDepth > 0,
                    t_replayedSystemDepth > 0, true, tracker != 0, carried))
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

        bool GuardedCompare(const void* target, const std::uint8_t* signature, std::size_t size)
        {
            return replay::mem::Matches(target, signature, size);
        }

        template <typename Fn>
        bool Hook(std::uint8_t* base, std::uintptr_t rva, const std::uint8_t* signature, std::size_t size,
            void* detour, std::atomic<Fn>& original, void*& target, const char* name, std::string& error)
        {
            void* address = base + rva;
            if (!GuardedCompare(address, signature, size))
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
        Hook(base, kSoundParamsCtorRva, kSoundParamsCtorSignature, sizeof(kSoundParamsCtorSignature),
            reinterpret_cast<void*>(&SoundParamsCtorDetour), g_originalCtor, g_ctorTarget, "sound_params", error);
        Hook(base, kSpeechSayRva, kSpeechSaySignature, sizeof(kSpeechSaySignature),
            reinterpret_cast<void*>(&SpeechSayDetour), g_originalSay, g_sayTarget, "speech_say", error);
        if (GuardedCompare(base + kFireEnqueueRva, kFireEnqueueSignature, sizeof(kFireEnqueueSignature)))
            g_fireDirect = reinterpret_cast<FireEnqueueFn>(base + kFireEnqueueRva);
        Hook(base, kSpeechCoreRva, kSpeechCoreSignature, sizeof(kSpeechCoreSignature),
            reinterpret_cast<void*>(&SpeechCoreDetour), g_originalCore, g_coreTarget, "speech_core", error);
        // The scope release the core calls first (0x2FA1137), for the puppet's
        // own speech refusal.
        if (!g_coreTarget)
            g_ownSpeechError = "speech_core_not_hooked";
        else if (!GuardedCompare(base + kSpeechScopeReleaseRva, kSpeechScopeReleaseSignature, sizeof(kSpeechScopeReleaseSignature)))
            g_ownSpeechError = "scope_release_signature_mismatch";
        else
        {
            g_ownSpeechRelease.store(reinterpret_cast<SpeechScopeReleaseFn>(base + kSpeechScopeReleaseRva),
                std::memory_order_release);
            g_ownSpeechError = "";
        }
        Hook(base, kFootstepRva, kFootstepSignature, sizeof(kFootstepSignature),
            reinterpret_cast<void*>(&FootstepDetour), g_originalFootstep, g_footstepTarget, "footstep", error);
        Hook(base, kPedAnimHandlerRva, kPedAnimHandlerSignature, sizeof(kPedAnimHandlerSignature),
            reinterpret_cast<void*>(&PedAnimHandlerDetour), g_originalPedAnim, g_pedAnimTarget, "ped_anim_events", error);
        Hook(base, kExplosionEnqueueRva, kExplosionEnqueueSignature, sizeof(kExplosionEnqueueSignature),
            reinterpret_cast<void*>(&ExplosionEnqueueDetour), g_originalExplosion, g_explosionTarget, "explosion_enqueue", error);
        Hook(base, kMovementUpdateRva, kMovementUpdateSignature, sizeof(kMovementUpdateSignature),
            reinterpret_cast<void*>(&MovementUpdateDetour), g_originalMovementUpdate, g_movementUpdateTarget,
            "movement_update", error);
        // The second queue's entry, after its store, generator and drain sites
        // (a mismatch leaves it off and the replay of the secondary steps with
        // it).
        if (!GuardedCompare(base + footevent::kEnqueueStoreRva, footevent::kEnqueueStoreSignature,
                sizeof(footevent::kEnqueueStoreSignature)) ||
            !GuardedCompare(base + footevent::kGeneratorCallRva, footevent::kGeneratorCallSignature,
                sizeof(footevent::kGeneratorCallSignature)) ||
            !GuardedCompare(base + footevent::kDrainCallRva, footevent::kDrainCallSignature,
                sizeof(footevent::kDrainCallSignature)))
        {
            g_footEventError = "foot_event_site_mismatch";
            error += "foot_event_site_mismatch;";
        }
        else if (Hook(base, footevent::kEnqueueRva, footevent::kEnqueueSignature, sizeof(footevent::kEnqueueSignature),
                     reinterpret_cast<void*>(&FootEventDetour), g_originalFootEvent, g_footEventTarget, "foot_event", error))
            g_footEventError = "";
        else
            g_footEventError = "foot_event_hook_failed";
        Hook(base, kSpeechLineStopRva, kSpeechLineStopSignature, sizeof(kSpeechLineStopSignature),
            reinterpret_cast<void*>(&SpeechLineStopDetour), g_originalStop, g_stopTarget, "speech_line_stop", error);
        Hook(base, kSpeechLineAudibleRva, kSpeechLineAudibleSignature, sizeof(kSpeechLineAudibleSignature),
            reinterpret_cast<void*>(&SpeechLineAudibleDetour), g_originalAudible, g_audibleTarget,
            "speech_line_start", error);
        Hook(base, kFireEnqueueRva, kFireEnqueueSignature, sizeof(kFireEnqueueSignature),
            reinterpret_cast<void*>(&FireEnqueueDetour), g_originalFire, g_fireTarget, "fire_enqueue", error);
        Hook(base, kMeleeImpactRva, kMeleeImpactSignature, sizeof(kMeleeImpactSignature),
            reinterpret_cast<void*>(&MeleeImpactDetour), g_originalMeleeImpact, g_meleeImpactTarget,
            "melee_impact", error);
        // 0x318EC48 takes exactly ten arguments (both callers pass ten; it
        // reads its frame up to entry rsp+0x50 and no xmm argument): the
        // detour declares and forwards all ten. A four-argument detour
        // crashes the game (seen on a knife execution).
        Hook(base, kStopAndForgetRva, kStopAndForgetSignature, sizeof(kStopAndForgetSignature),
            reinterpret_cast<void*>(&StopAndForgetDetour), g_originalStopAndForget, g_stopAndForgetTarget,
            "stop_and_forget", error);
        Hook(base, kPersistentCreateRva, kPersistentCreateSignature, sizeof(kPersistentCreateSignature),
            reinterpret_cast<void*>(&PersistentCreateDetour), g_originalPersistentCreate, g_persistentCreateTarget,
            "persistent_create", error);
        Hook(base, kPersistentByRefRva, kPersistentByRefSignature, sizeof(kPersistentByRefSignature),
            reinterpret_cast<void*>(&PersistentByRefDetour), g_originalByRef, g_byRefTarget, "persistent_by_ref", error);
        Hook(base, kPersistentByHashRva, kPersistentByHashSignature, sizeof(kPersistentByHashSignature),
            reinterpret_cast<void*>(&PersistentByHashDetour), g_originalByHash, g_byHashTarget, "persistent_by_hash", error);
        Hook(base, kCreateAndPlayRva, kCreateAndPlaySignature, sizeof(kCreateAndPlaySignature),
            reinterpret_cast<void*>(&CreateAndPlayDetour), g_originalCreateAndPlay, g_createAndPlayTarget,
            "create_and_play", error);
        Hook(base, kSoundRequestRva, kSoundRequestSignature, sizeof(kSoundRequestSignature),
            reinterpret_cast<void*>(&SoundRequestDetour), g_originalSoundRequest, g_soundRequestTarget,
            "sound_request", error);
        Hook(base, kMeleeBodyImpactRva, kMeleeBodyImpactSignature, sizeof(kMeleeBodyImpactSignature),
            reinterpret_cast<void*>(&MeleeBodyImpactDetour), g_originalBodyImpact, g_bodyImpactTarget,
            "melee_body_impact", error);
        InstallPlayerWeaponAudio(error);
        InstallPlayerMountAudio(error);
        InstallMovementVolume(error);
        return g_ctorTarget || g_sayTarget;
    }

    namespace
    {
        bool GuardedPedAudio(void* ped, std::uint8_t*& audio)
        {
            static PedAudioGetterFn getter = nullptr;
            if (!getter)
            {
                auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
                if (!GuardedCompare(base + kPedAudioGetterRva, kPedAudioGetterSignature,
                        sizeof(kPedAudioGetterSignature)))
                {
                    return false;
                }
                getter = reinterpret_cast<PedAudioGetterFn>(base + kPedAudioGetterRva);
            }
            __try
            {
                audio = getter(ped);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    // Read-only sampling of the positional emitter manager (see
    // audio_observer.h EmitterSlot).
    namespace
    {
        // 0x14184B4: mov rdx, rbx; lea rcx, [rip + manager]; call 0x3212028.
        constexpr std::uintptr_t kEmitterSiteRva = 0x14184B4;
        constexpr std::uint8_t kEmitterSiteHead[] = {0x48, 0x8B, 0xD3, 0x48, 0x8D, 0x0D};
        constexpr std::size_t kEmitterSiteDisp = 6;
        constexpr std::size_t kEmitterSiteCall = 10;
        constexpr std::uintptr_t kEmitterStartRva = 0x3212028;
        constexpr std::uint8_t kEmitterStartSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC9};
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
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            std::uint8_t* site = base + kEmitterSiteRva;
            std::uint8_t call = 0;
            if (!GuardedCompare(site, kEmitterSiteHead, sizeof(kEmitterSiteHead)) ||
                !GuardedRead(&call, site + kEmitterSiteCall, 1) || call != 0xE8)
            {
                g_emitterError = "emitter_site_mismatch";
                error = g_emitterError;
                return false;
            }
            const auto rip = [](std::uint8_t* at, std::size_t dispOffset) {
                std::int32_t disp = 0;
                std::memcpy(&disp, at + dispOffset, sizeof(disp));
                return at + dispOffset + sizeof(disp) + disp;
            };
            if (rip(site, kEmitterSiteCall + 1) != base + kEmitterStartRva ||
                !GuardedCompare(base + kEmitterStartRva, kEmitterStartSignature, sizeof(kEmitterStartSignature)))
            {
                g_emitterError = "emitter_start_mismatch";
                error = g_emitterError;
                return false;
            }
            g_emitterManager = rip(site, kEmitterSiteDisp);
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
        // The engine's tracker getter (0xE3B394) returns this component +0x10.
        const std::uint64_t component = PedAudioComponent(pedEntity);
        return component ? component + 0x10 : 0;
    }

    // The ped's row in the component table (slot +0x30 is the audio
    // component); 0 when the getter's signature or a read fails.
    std::uint64_t PedComponentRow(void* pedEntity)
    {
        static int verified = -1;
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (verified < 0)
            verified = GuardedCompare(base + kComponentGetterRva, kComponentGetterSignature,
                sizeof(kComponentGetterSignature)) ? 1 : 0;
        if (!verified || !pedEntity)
            return 0;
        std::uint32_t index = 0;
        std::uint32_t indexBase = 0;
        std::uint64_t table = 0;
        if (!GuardedRead(&index, static_cast<std::uint8_t*>(pedEntity) + 0x9C, 4) ||
            !GuardedRead(&indexBase, base + kComponentIndexBaseRva, 4) ||
            !GuardedRead(&table, base + kComponentTableRva, 8) || !table)
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
        // Foot component (constructor 0x3178EB4 writes this vtable and
        // clears +0x450 bits 0 and 1). A ped without component slot +0x00
        // (every NPC, so every puppet) gets footstep audio (+0x599) only
        // through a global table of two slots per ped category (0x5C7FC30); a
        // newcomer evicts the occupant (0x31A2770 clears its +0x599 and asks
        // the foot off), which makes puppets lose their footsteps mid clip.
        // 0x319D2BC skips that table and sets +0x599 itself when +0x450 bit 1
        // is set; no engine code sets that bit, and the only other reader is
        // 0x8E4C58 (with +0x448).
        constexpr std::uintptr_t kFootVtableRva = 0x38389B0;
        constexpr std::uintptr_t kFootForceByte = 0x450;
        constexpr std::uint8_t kFootForceAudio = 2;

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
        const std::uint64_t expected = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr)) + kFootVtableRva;
        std::uint64_t vtable = 0;
        if (!ReadU64(foot, vtable) || vtable != expected)
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

    // The audio LOD override of a puppet's holder. 0x317BABC (LOD of the
    // holder's manager, holder+0x4440) starts at 4, then
    // `cmp dword [holder+0x19A4], 4; je` at 0x317BB0F: any other value is the
    // LOD (0x317BB1B), and it reports the change (byte [rdx] = 1), after which
    // 0x31A3428 calls 0x318A858, which asks the activation of every enabled
    // component whose max LOD the new LOD fits. holder+0x19A4 is foot+0x444
    // (foot = holder+0x1560); the foot constructor 0x3178EB4 writes 4 there
    // (no override), so a new ped in the same pool block starts without it.
    // A guarded write on the replay's own puppet after the foot vtable check;
    // any LOD, 4 writes the override off.
    bool SetPuppetAudioLodValue(void* pedEntity, std::uint8_t lod, std::string& status)
    {
        const std::uint64_t holder = PedAudioHolder(pedEntity);
        if (!holder)
        {
            status = "no_holder";
            return false;
        }
        const std::uint64_t foot = holder + kFootAudioOffset;
        const std::uint64_t expected = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr)) + kFootVtableRva;
        std::uint64_t vtable = 0;
        if (!ReadU64(foot, vtable) || vtable != expected)
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

    bool AudioGlobalFlags(std::uint32_t& flags)
    {
        flags = 0;
        const auto base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
        return base && ReadBytes(base + 0x5B9CFC0, &flags, 4);
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
        SpeechParamsFn g_speechCtor = nullptr;
        SpeechParamsFn g_speechDtor = nullptr;
        SpeechDirectFn g_speechDirect = nullptr;
        SpeechScopeFn g_scopeCtor = nullptr;
        SpeechScopeFn g_scopeRelease = nullptr;
        SpeechScopeFn g_scopeDtor = nullptr;
        SpeechLineStartFn g_lineStart = nullptr;

        // The stop function to call from the script thread: the detour's
        // trampoline when the stop is hooked (signature already checked),
        // else the function itself after its signature check. Null when the
        // signature does not match (mismatched lines are then left alone
        // and only logged).
        SpeechLineStopFn LineStopFunction()
        {
            if (const SpeechLineStopFn original = g_originalStop.load(std::memory_order_acquire))
                return original;
            static SpeechLineStopFn direct = nullptr;
            static bool checked = false;
            if (!checked)
            {
                checked = true;
                auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
                if (GuardedCompare(base + kSpeechLineStopRva, kSpeechLineStopSignature, sizeof(kSpeechLineStopSignature)))
                    direct = reinterpret_cast<SpeechLineStopFn>(base + kSpeechLineStopRva);
            }
            return direct;
        }

        struct DirectEvidence
        {
            bool attached = false;
            std::int32_t stateBefore = -1;
            std::int32_t stateAfter = -1;
            std::int64_t startResult = -1;
        };

        // Identity of the line the scope holds (all reads inside the
        // caller's SEH frame).
        void ReadPlayedLine(std::uint8_t* line, PlayedLine& played)
        {
            played.line = reinterpret_cast<std::uint64_t>(line);
            auto* data = *reinterpret_cast<std::uint8_t**>(line + 0x1D8);
            if (!data)
                return;
            played.voice = *reinterpret_cast<std::uint32_t*>(data + 0x110);
            played.context = *reinterpret_cast<std::uint32_t*>(data + 0x114);
            played.variation = *reinterpret_cast<std::int32_t*>(data + 0x118);
            played.dataType = *reinterpret_cast<std::uint32_t*>(data + 0xE0);
            // speech_slots.h: the voice source the slot was allocated by
            // (line +0x218 = params +0xF8, 0x2FABD75).
            played.source = *reinterpret_cast<std::int32_t*>(line + 0x218);
            // The request fields the line init copied (inside the caller's SEH
            // frame like the reads above).
            replay::speech::request::FromLineBlock(line + replay::speech::request::kLineBlockFrom,
                replay::speech::request::kLineBlockBytes, played.request);
            // The sound set the line plays from.
            replay::speech::request::SetFromDataBlock(data + replay::speech::request::kDataSetFrom,
                replay::speech::request::kDataSetBytes, played.request);
            auto* voiceObject = *reinterpret_cast<std::uint8_t**>(line + 0x1E8);
            if (voiceObject)
            {
                played.voiceObject = reinterpret_cast<std::uint64_t>(voiceObject);
                played.voiceObjectType = voiceObject[0];
                played.voiceObjectPartial = *reinterpret_cast<std::uint32_t*>(voiceObject + 8);
            }
            played.identityRead = true;
        }

        // speech_conversation.h: the SpeechParams applier's own calls for the
        // sound set (0x2F967E8 at 0x2F9680B..0x2F9684D), each checked byte for
        // byte with the applier's call sites and writes, the context applier's
        // keep test (it fills +0xC0 only while it is 0) and the data copy the
        // recorder reads. A mismatch disables the set for the session (nothing
        // written, status disabled) and names the site
        // (SPEECH_CONVERSATION_CHECK).
        struct SetCheckSite
        {
            const char* name;
            std::uintptr_t rva;
            std::uint8_t size;
            std::uint8_t bytes[16];
        };
        constexpr std::uintptr_t kSetMetadataRva = 0x252AD8C;  // lea rax, [metadata 0x5342018]; ret
        constexpr std::uintptr_t kSetRefByHashRva = 0x252B408; // (metadata, out, hash, -1): *out = chunk << 24 | offset, -1 if none
        constexpr std::uintptr_t kSetObjectRva = 0x252B194;    // ({ref, 0xFFFFF}): the type 0x29 object of the ref, or null
        constexpr SetCheckSite kSetCheckSites[] = {
            {"metadata_prologue", kSetMetadataRva, 8, {0x48, 0x8D, 0x05, 0x85, 0x72, 0xE1, 0x02, 0xC3}},
            {"ref_by_hash_prologue", kSetRefByHashRva, 16,
                {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x41, 0x8B, 0xC0, 0xC6, 0x44, 0x24, 0x28, 0x00, 0x44, 0x89}},
            {"object_prologue", kSetObjectRva, 16,
                {0x48, 0x83, 0xEC, 0x28, 0x45, 0x33, 0xC9, 0x8B, 0xD1, 0x48, 0x8D, 0x0D, 0x74, 0x6E, 0xE1, 0x02}},
            {"applier_hash_write", 0x2F9680B, 6, {0x89, 0xA9, 0xC8, 0x00, 0x00, 0x00}},
            {"applier_metadata_call", 0x2F96811, 5, {0xE8, 0x76, 0x45, 0x59, 0xFF}},
            {"applier_ref_call", 0x2F96825, 5, {0xE8, 0xDE, 0x4B, 0x59, 0xFF}},
            {"applier_ref_write", 0x2F96830, 6, {0x89, 0x8B, 0xCC, 0x00, 0x00, 0x00}},
            {"applier_object_call", 0x2F96843, 5, {0xE8, 0x4C, 0x49, 0x59, 0xFF}},
            {"applier_object_write", 0x2F9684D, 7, {0x48, 0x89, 0x83, 0xC0, 0x00, 0x00, 0x00}},
            {"context_keeps_object", 0x2F965F3, 8, {0x48, 0x39, 0xA9, 0xC0, 0x00, 0x00, 0x00, 0x75}},
            {"data_copies_set", 0x2FA9327, 15,
                {0x0F, 0x10, 0x86, 0xC0, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x7F, 0x87, 0xE8, 0x00, 0x00, 0x00}},
        };
        // The applier's rel32 calls land on the three functions above.
        constexpr std::uintptr_t CallTarget(const SetCheckSite& site)
        {
            return static_cast<std::uint32_t>(site.rva + 5u +
                (static_cast<std::uint32_t>(site.bytes[1]) | (static_cast<std::uint32_t>(site.bytes[2]) << 8) |
                    (static_cast<std::uint32_t>(site.bytes[3]) << 16) | (static_cast<std::uint32_t>(site.bytes[4]) << 24)));
        }
        static_assert(CallTarget(kSetCheckSites[4]) == kSetMetadataRva, "applier metadata call target");
        static_assert(CallTarget(kSetCheckSites[5]) == kSetRefByHashRva, "applier ref call target");
        static_assert(CallTarget(kSetCheckSites[7]) == kSetObjectRva, "applier object call target");

        bool g_setChecked = false;
        bool g_setOk = false;
        const char* g_setFailed = "unchecked";

        bool EnsureSetCalls()
        {
            if (g_setChecked)
                return g_setOk;
            g_setChecked = true;
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            g_setFailed = "none";
            for (const SetCheckSite& site : kSetCheckSites)
            {
                if (!GuardedCompare(base + site.rva, site.bytes, site.size))
                {
                    g_setFailed = site.name;
                    break;
                }
            }
            g_setOk = std::strcmp(g_setFailed, "none") == 0;
            return g_setOk;
        }

        using SetMetadataFn = void*(__fastcall*)();
        using SetRefByHashFn = std::uint32_t*(__fastcall*)(void* metadata, std::uint32_t* out, std::uint32_t hash,
            std::int32_t typeFilter);
        using SetObjectFn = void*(__fastcall*)(std::uint64_t ref);

        // 0x2F96811..0x2F9682C: the metadata, then the ref of the hash.
        bool GuardedSetRefByHash(std::uint8_t* base, std::uint32_t hash, std::uint32_t& ref, std::uint32_t& code)
        {
            __try
            {
                void* metadata = reinterpret_cast<SetMetadataFn>(base + kSetMetadataRva)();
                std::uint32_t out[2] = {0xFFFFFFFFu, replay::speech::conversation::kRefHighAny};
                const std::uint32_t* got = reinterpret_cast<SetRefByHashFn>(base + kSetRefByHashRva)(metadata, out, hash, -1);
                ref = got ? *got : 0xFFFFFFFFu;
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // 0x2F96836..0x2F96848: the set object of the ref ({ref, 0xFFFFF}).
        bool GuardedSetObject(std::uint8_t* base, std::uint32_t ref, std::uint64_t& object, std::uint32_t& code)
        {
            __try
            {
                const std::uint64_t key = (static_cast<std::uint64_t>(replay::speech::conversation::kRefHighAny) << 32) | ref;
                object = reinterpret_cast<std::uint64_t>(reinterpret_cast<SetObjectFn>(base + kSetObjectRva)(key));
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The object of a recorded ref (no name looked up) checked before it
        // goes into params: its count and name table read the way 0x49F6E4
        // reads them, the hashes ascending (speech_conversation.h
        // SetTableAscending). A real set passes; a ref from another metadata
        // layout that landed on a 0x29 byte inside another object does not.
        // Read with the plain floor (replay::mem::kLowest): the pointer is the
        // engine's own lookup result, bounded by 0x2516734 to a loaded chunk.
        bool SetObjectLooksReal(std::uint64_t object)
        {
            using namespace replay::speech::conversation;
            std::uint32_t count = 0;
            if (!replay::mem::Copy(&count, static_cast<std::uintptr_t>(object + kSetCountOffset), sizeof(count)) ||
                count == 0 || count > kSetMaxEntries)
                return false;
            std::vector<std::uint8_t> table(static_cast<std::size_t>(count) * kSetEntryBytes);
            if (!replay::mem::Copy(table.data(), static_cast<std::uintptr_t>(object + kSetEntriesOffset), table.size()))
                return false;
            return SetTableAscending(table.data(), count);
        }

        // What GuardedSayDirect writes into params after the ctor.
        struct SetWrite
        {
            bool write = false;
            std::uint32_t hash = 0;
            std::uint32_t ref = 0;
            std::uint64_t object = 0;
        };

        // Resolves the asked set the applier's way, on the script thread,
        // before the request (an exception here leaves the line as before).
        // Only a found object is written: without one the applier leaves
        // +0xC0 at 0 and the context applier then overwrites +0xC8/+0xCC.
        replay::speech::conversation::SetStatus ResolveSoundSet(const replay::speech::request::Ask& ask, SetWrite& out)
        {
            using replay::speech::conversation::SetStatus;
            out = SetWrite{};
            if (ask.setFrom == replay::speech::request::SetFrom::None)
                return SetStatus::NotAsked;
            if (!EnsureSetCalls())
                return SetStatus::Disabled;
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            std::uint32_t code = 0;
            std::uint32_t hashRef = 0xFFFFFFFFu;
            bool hashLooked = false;
            if (ask.setHash)
            {
                if (!GuardedSetRefByHash(base, ask.setHash, hashRef, code))
                    return SetStatus::Exception;
                hashLooked = true;
            }
            bool byHash = false;
            const std::uint32_t ref = replay::speech::conversation::RefToResolve(ask, hashLooked, hashRef, byHash);
            if (!ref)
                return SetStatus::NoObject;
            std::uint64_t object = 0;
            if (!GuardedSetObject(base, ref, object, code))
                return SetStatus::Exception;
            if (!object)
                return SetStatus::NoObject;
            // By hash the object is the applier's own chain; by a recorded
            // ref it is checked first (a bad one would be read by the line's
            // set searches on the audio side, outside this guard).
            if (!byHash && !SetObjectLooksReal(object))
                return SetStatus::BadObject;
            out.write = true;
            out.hash = ask.setHash; // the applier writes the hash it was given (0 from the sounds)
            out.ref = ref;
            out.object = object;
            return byHash ? SetStatus::ByHash : SetStatus::ByRef;
        }

        bool GuardedSayDirect(std::uint8_t* speech, std::uint8_t* params, std::uint8_t* scope,
            std::uint32_t voice, std::uint32_t context, std::int32_t variation,
            const replay::speech::Identity* expected, SpeechLineStopFn stop,
            std::uint32_t& result, DirectEvidence& evidence, PlayedLine& played, std::uint32_t& code,
            std::uint32_t startOffsetMs, std::int32_t source, const replay::speech::request::Ask* ask,
            const SetWrite* set)
        {
            __try
            {
                g_speechCtor(params);
                // speech_request.h: the recorded line type (+0xF4, 0 after the
                // ctor: the validator 0x2F9A7BB would write 2) and the recorded
                // request fields, each already range checked by BuildAsk.
                if (ask)
                {
                    if (ask->lineType)
                        *reinterpret_cast<std::uint32_t*>(params + replay::speech::request::kParamsLineType) = ask->lineType;
                    for (std::uint8_t n = 0; n < ask->count && n < replay::speech::request::kFieldCount; ++n)
                        *reinterpret_cast<std::uint32_t*>(params + ask->writes[n].offset) = ask->writes[n].value;
                    // speech_script_path.h: flag bytes the line init turns into
                    // line bits (the duck byte +0x19E).
                    for (std::uint8_t n = 0; n < ask->byteCount && n < replay::speech::request::kMaxByteWrites; ++n)
                        if (ask->bytes[n].offset < 0x200)
                            params[ask->bytes[n].offset] = ask->bytes[n].value;
                }
                // speech_conversation.h: the recorded sound set, the applier's
                // three writes (0x2F9680B, 0x2F96830, 0x2F9684D); the copy
                // 0x2F922F4 keeps them and the context applier skips a params
                // with +0xC0 set (0x2F965F3).
                if (set && set->write)
                {
                    *reinterpret_cast<std::uint32_t*>(params + replay::speech::conversation::kParamsSetHash) = set->hash;
                    *reinterpret_cast<std::uint32_t*>(params + replay::speech::conversation::kParamsSetRef) = set->ref;
                    *reinterpret_cast<std::uint64_t*>(params + replay::speech::conversation::kParamsSetObject) = set->object;
                }
                // speech_slots.h: the ctor leaves +0xF8 at 0 (0x2FAED9D zeroes
                // +0xF4..+0xFB) and the context metadata only fills it when
                // still 0 (0x2F9665E): 4 asks an ambient slot by priority with
                // the line's own wave, instead of the bank reuse of source 3.
                if (source > 0)
                    *reinterpret_cast<std::int32_t*>(params + replay::speech::slots::kParamsSource) = source;
                *reinterpret_cast<std::uint32_t*>(params + kParamsContext) = context;
                *reinterpret_cast<std::uint32_t*>(params + kParamsVoice) = voice;
                if (startOffsetMs) // the ctor left 0
                    *reinterpret_cast<std::uint32_t*>(params + kParamsStartOffset) = startOffsetMs;
                if (variation > 0)
                {
                    *reinterpret_cast<std::int32_t*>(params + kParamsVariation) = variation;
                    params[0x192] = 0;
                    params[0x198] = 0;
                }
                // +0x18D is not set: in the variation choice (0x2F9EA2C) it only
                // enables a random re-draw when the recorded variation is
                // refused. Skip the recently-played check: the params byte
                // +0x18C becomes the line's bit (+0x2BC bit 7) that the repeat
                // check (0x2F97944, code 22) tests before refusing a line heard
                // moments ago, as the recorded one always was; a replay ignores
                // the speech history. Without it every replayed line is stopped
                // in state 3. It also enables the re-draw, so the created line
                // is compared below before it may start.
                params[0x18C] = 1;
                g_scopeCtor(scope);
                result = g_speechDirect(speech, scope, params);
                // As the AI step does after the core: start the line the
                // scope now holds, unless it is in state 6 or it is not the
                // recorded line.
                auto* line = *reinterpret_cast<std::uint8_t**>(scope + 0x28);
                evidence.attached = line != nullptr;
                if (line)
                {
                    evidence.stateBefore = *reinterpret_cast<std::int32_t*>(line + 0x234);
                    ReadPlayedLine(line, played);
                    if (expected && played.identityRead)
                    {
                        replay::speech::Identity got;
                        got.voice = played.voice;
                        got.context = played.context;
                        got.variation = played.variation;
                        got.voiceObjectHash = played.voiceObject ? replay::speech::Finalize(played.voiceObjectPartial) : 0;
                        got.voiceObjectType = played.voiceObjectType;
                        played.mismatch = replay::speech::Compare(*expected, got);
                    }
                    else if (expected)
                    {
                        played.mismatch = replay::speech::kMismatchVoice | replay::speech::kMismatchContext |
                            replay::speech::kMismatchVariation;
                    }
                }
                if (line && played.mismatch && stop && *reinterpret_cast<std::int32_t*>(line + 0x234) != 6)
                {
                    stop(line, 2);
                    played.stoppedOnMismatch = true;
                }
                else if (line && !played.mismatch && *reinterpret_cast<std::int32_t*>(line + 0x234) != 6)
                {
                    evidence.startResult = g_lineStart(line);
                }
                if (line)
                    evidence.stateAfter = *reinterpret_cast<std::int32_t*>(line + 0x234);
                g_scopeRelease(scope);
                g_scopeDtor(scope);
                g_speechDtor(params);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    namespace
    {
        std::uint8_t* g_voiceStore = nullptr;
        void* g_voiceCallback = nullptr;
        VoiceLookupFn g_voiceLookup = nullptr;
        VoiceApplyFn g_voiceApply = nullptr;
        bool g_voiceChecked = false;
        std::string g_voiceCheckError;

        std::uint8_t* RipTarget(std::uint8_t* function, std::size_t dispOffset)
        {
            std::int32_t disp = 0;
            std::memcpy(&disp, function + dispOffset, sizeof(disp));
            return function + dispOffset + sizeof(disp) + disp;
        }

        struct VoiceCallback
        {
            void* target = nullptr;
            std::uint32_t value = 0;
        };

        // The setter's bytes, then the lookup and apply targets they call:
        // each must be the documented RVA with the documented prologue.
        bool EnsureVoiceCalls(std::string& error)
        {
            if (g_voiceChecked)
            {
                error = g_voiceCheckError;
                return g_voiceApply != nullptr;
            }
            g_voiceChecked = true;
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            std::uint8_t* setter = base + kVoiceSetterRva;
            if (!GuardedCompare(setter, kVoiceSetterSignature, sizeof(kVoiceSetterSignature)))
            {
                g_voiceCheckError = error = "voice_setter_signature_mismatch";
                return false;
            }
            std::uint8_t* lookup = RipTarget(setter, kVoiceLookupCall + 1);
            std::uint8_t* apply = RipTarget(setter, kVoiceApplyCall + 1);
            if (lookup != base + kVoiceLookupRva ||
                !GuardedCompare(lookup, kVoiceLookupSignature, sizeof(kVoiceLookupSignature)))
            {
                g_voiceCheckError = error = "voice_lookup_signature_mismatch";
                return false;
            }
            if (apply != base + kVoiceApplyRva ||
                !GuardedCompare(apply, kVoiceApplySignature, sizeof(kVoiceApplySignature)))
            {
                g_voiceCheckError = error = "voice_apply_signature_mismatch";
                return false;
            }
            g_voiceStore = RipTarget(setter, kVoiceStoreDisp);
            g_voiceCallback = RipTarget(setter, kVoiceCallbackDisp);
            g_voiceLookup = reinterpret_cast<VoiceLookupFn>(lookup);
            g_voiceApply = reinterpret_cast<VoiceApplyFn>(apply);
            return true;
        }

        // stage 1: lookup, 3: holder, 4: apply, 5: done. `source` 1 = typed
        // lookup (the only source).
        bool GuardedPinVoice(std::uint8_t* speech, std::uint32_t voiceHash, std::uint8_t voiceType,
            std::uint8_t* recordedObject, int& stage, int& source, std::uint32_t& code)
        {
            __try
            {
                stage = 1;
                void* voice = g_voiceLookup(g_voiceStore, voiceHash, voiceType);
                source = 1;
                // The recorded voice object pointer is never dereferenced: it
                // belongs to the session that recorded the clip and can point
                // at freed memory after a save load (same class as the decal
                // info pointer). Kept only in the log.
                (void)recordedObject;
                if (!voice)
                    return false;
                stage = 3;
                auto* vtable = *reinterpret_cast<VoiceHolderFn**>(speech);
                void* holder = vtable[0x128 / sizeof(void*)](speech);
                if (!holder)
                    return false;
                stage = 4;
                VoiceCallback callback;
                callback.target = g_voiceCallback;
                g_voiceApply(holder, voice, 1, 1, &callback);
                stage = 5;
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    namespace
    {
        bool GuardedVoiceLookup(std::uint32_t hash, std::uint8_t type, void*& object)
        {
            __try
            {
                object = g_voiceLookup(g_voiceStore, hash, type);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    namespace
    {
        constexpr std::uintptr_t kRefContainerRva = 0x252B194;
        constexpr std::uint8_t kRefContainerSignature[] = {0x48, 0x83, 0xEC, 0x28, 0x45, 0x33, 0xC9, 0x8B, 0xD1, 0x48, 0x8D, 0x0D};
        using RefContainerFn = void*(__fastcall*)(std::uint32_t);

        bool GuardedRefContainer(RefContainerFn fn, std::uint32_t key, void*& out)
        {
            __try
            {
                out = fn(key);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    std::uint64_t RefusedPlayRequests() { return g_refusedPlayRequests.load(std::memory_order_relaxed); }

    const char* IndirectRefState(std::uint64_t soundRef)
    {
        if (((soundRef >> 32) & 0xF00000) != 0x800000)
            return "direct";
        static int checked = -1;
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (checked < 0)
            checked = GuardedCompare(base + kRefContainerRva, kRefContainerSignature, sizeof(kRefContainerSignature)) ? 1 : 0;
        if (!checked)
            return "container_signature_mismatch";
        void* container = nullptr;
        if (!GuardedRefContainer(reinterpret_cast<RefContainerFn>(base + kRefContainerRva), static_cast<std::uint32_t>(soundRef), container))
            return "container_exception";
        return container ? "indirect_found" : "indirect_missing";
    }

    bool PinSpeechVoice(void* pedEntity, std::uint32_t voiceHash, std::uint8_t voiceType,
        std::uint64_t recordedObject, std::string& status)
    {
        if (!EnsureVoiceCalls(status))
            return false;
        std::uint8_t* speech = reinterpret_cast<std::uint8_t*>(SpeechEntity(pedEntity));
        if (!speech)
        {
            status = "no_speech_entity";
            return false;
        }
        int stage = 0;
        int source = 0;
        std::uint32_t code = 0;
        if (!GuardedPinVoice(speech, voiceHash, voiceType ? voiceType : 0x22,
                reinterpret_cast<std::uint8_t*>(recordedObject), stage, source, code))
        {
            char text[48]{};
            if (code)
                std::snprintf(text, sizeof(text), "exception_%08X_stage_%d", code, stage);
            else
                std::snprintf(text, sizeof(text), stage <= 2 ? "voice_not_found" : "no_voice_holder");
            status = text;
            // Evidence: which metadata type holds this name.
            if (stage <= 2 && !code)
            {
                static std::vector<std::uint32_t> probed;
                if (std::find(probed.begin(), probed.end(), voiceHash) == probed.end() && probed.size() < 64)
                {
                    probed.push_back(voiceHash);
                    char found[24] = "none";
                    for (int type = 1; type < 0x80; ++type)
                    {
                        void* object = nullptr;
                        if (GuardedVoiceLookup(voiceHash, static_cast<std::uint8_t>(type), object) && object)
                        {
                            std::snprintf(found, sizeof(found), "0x%02X", type);
                            break;
                        }
                    }
                    status += ",found_type=";
                    status += found;
                }
            }
            return false;
        }
        (void)source;
        status = "pinned_lookup";
        return true;
    }

    bool SpeechSoundSetCallsOk(std::string& failed)
    {
        const bool ok = EnsureSetCalls();
        failed = g_setFailed;
        return ok;
    }

    bool PlaySpeechDirect(void* pedEntity, std::uint32_t voiceHash, std::uint32_t contextPartialHash,
        std::int32_t variation, const replay::speech::Identity* expected, std::uint32_t& result,
        PlayedLine& played, std::string& error, std::uint32_t startOffsetMs, std::int32_t source,
        const replay::speech::request::Ask* ask)
    {
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!g_speechDirect)
        {
            if (!GuardedCompare(base + kSpeechParamsCtorRva, kSpeechParamsCtorSignature, sizeof(kSpeechParamsCtorSignature)) ||
                !GuardedCompare(base + kSpeechParamsDtorRva, kSpeechParamsDtorSignature, sizeof(kSpeechParamsDtorSignature)) ||
                !GuardedCompare(base + kSpeechDirectRva, kSpeechDirectSignature, sizeof(kSpeechDirectSignature)) ||
                !GuardedCompare(base + kSpeechScopeCtorRva, kSpeechScopeCtorSignature, sizeof(kSpeechScopeCtorSignature)) ||
                !GuardedCompare(base + kSpeechScopeReleaseRva, kSpeechScopeReleaseSignature, sizeof(kSpeechScopeReleaseSignature)) ||
                !GuardedCompare(base + kSpeechScopeDtorRva, kSpeechScopeDtorSignature, sizeof(kSpeechScopeDtorSignature)) ||
                !GuardedCompare(base + kSpeechLineStartRva, kSpeechLineStartSignature, sizeof(kSpeechLineStartSignature)))
            {
                error = "speech_direct_signature_mismatch";
                return false;
            }
            g_speechCtor = reinterpret_cast<SpeechParamsFn>(base + kSpeechParamsCtorRva);
            g_speechDtor = reinterpret_cast<SpeechParamsFn>(base + kSpeechParamsDtorRva);
            g_scopeCtor = reinterpret_cast<SpeechScopeFn>(base + kSpeechScopeCtorRva);
            g_scopeRelease = reinterpret_cast<SpeechScopeFn>(base + kSpeechScopeReleaseRva);
            g_scopeDtor = reinterpret_cast<SpeechScopeFn>(base + kSpeechScopeDtorRva);
            g_lineStart = reinterpret_cast<SpeechLineStartFn>(base + kSpeechLineStartRva);
            g_speechDirect = reinterpret_cast<SpeechDirectFn>(base + kSpeechDirectRva);
        }
        std::uint8_t* speech = reinterpret_cast<std::uint8_t*>(SpeechEntity(pedEntity));
        if (!speech)
        {
            error = "no_speech_entity";
            return false;
        }
        alignas(16) std::uint8_t params[0x200] = {};
        // The Say wrapper keeps the scope in 0x40 bytes of its frame.
        alignas(16) std::uint8_t scope[0x80] = {};
        std::uint32_t code = 0;
        DirectEvidence evidence;
        played = PlayedLine{};
        const SpeechLineStopFn stop = LineStopFunction();
        // The asked sound set, resolved first.
        SetWrite set;
        const replay::speech::conversation::SetStatus setStatus =
            ask ? ResolveSoundSet(*ask, set) : replay::speech::conversation::SetStatus::NotAsked;
        const bool said = GuardedSayDirect(speech, params, scope, voiceHash, contextPartialHash, variation, expected, stop,
            result, evidence, played, code, startOffsetMs, source, ask, &set);
        played.setStatus = static_cast<std::uint8_t>(setStatus);
        played.setWrittenRef = set.write ? set.ref : 0;
        if (!said)
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        char text[128]{};
        std::snprintf(text, sizeof(text), "result_%u|attached_%d|state_%d_to_%d|start_%lld%s", result,
            evidence.attached ? 1 : 0, evidence.stateBefore, evidence.stateAfter,
            static_cast<long long>(evidence.startResult),
            played.mismatch ? (played.stoppedOnMismatch ? "|mismatch_stopped" : "|mismatch_no_stop") : "");
        error = text;
        return true;
    }

    // An animal's vocalization asked the way the native
    // PLAY_ANIMAL_VOCALIZATION's handler 0xF72394 asks it, with the context
    // hash itself instead of a name: the speech params (ctor 0x2F91EA4),
    // +0x18D = 1 and +0x5C = 1 as the handler sets them, +0x130 = 0 (its third
    // argument), then the animal speech entity's vtable 0x37FE430 slot +0x288
    // (0x2FB2B4C: writes the 32-bit context into params +0x150, +0x130 from
    // +0x18D, and enters the Say wrapper 0x2F9FC84, which runs the animal
    // pipeline and the AI step that starts the line), and the params dtor
    // 0x143E98. The native's slot +0x290 (0x2FB2B70) hashes the name and jumps
    // there. Checked (speech_identity.h CheckAnimalVocalEntry): the vtable is
    // the animal one, its slot is the expected function with its prologue.
    namespace
    {
        constexpr std::uintptr_t kAnimalSpeechVtableRva = replay::speech::kAnimalSpeechVtableRva;
        constexpr std::size_t kAnimalVocalSlot = replay::speech::kAnimalVocalHashSlot;
        constexpr std::uintptr_t kAnimalVocalRva = replay::speech::kAnimalVocalHashRva;
        constexpr std::uint8_t kAnimalVocalSignature[] = {
            0x33, 0xC0,                               // xor eax, eax
            0x41, 0x89, 0x90, 0x50, 0x01, 0x00, 0x00, // mov [r8+150h], edx
            0x41, 0x38, 0x80, 0x8D, 0x01, 0x00, 0x00}; // cmp [r8+18Dh], al
        using AnimalVocalFn = std::uint32_t(__fastcall*)(void* speech, std::uint32_t context, void* params);

        bool GuardedAnimalVocal(AnimalVocalFn vocal, SpeechParamsFn ctor, SpeechParamsFn dtor, std::uint8_t* speech,
            std::uint8_t* params, std::uint32_t context, std::uint32_t voice, std::int32_t variation,
            std::uint32_t& result, std::uint32_t& code)
        {
            __try
            {
                ctor(params);
                params[0x18D] = 1;
                *reinterpret_cast<std::uint32_t*>(params + 0x5C) = 1;
                *reinterpret_cast<std::uint32_t*>(params + 0x130) = 0;
                // The recorded voice and variation, kept by the animal
                // pipeline's choice (0x2F9DAA8 / 0x2F9DB84 keep a non-zero
                // +0x14C and a positive +0x158), as the direct path sets them.
                if (voice)
                    *reinterpret_cast<std::uint32_t*>(params + kParamsVoice) = voice;
                if (variation > 0)
                {
                    *reinterpret_cast<std::int32_t*>(params + kParamsVariation) = variation;
                    params[0x192] = 0;
                    params[0x198] = 0;
                }
                result = vocal(speech, context, params);
                dtor(params);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool PlayAnimalVocal(void* pedEntity, std::uint32_t contextPartialHash, std::uint32_t voiceHash,
        std::int32_t variation, std::uint32_t& result, std::string& error)
    {
        result = 0;
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!GuardedCompare(base + kSpeechParamsCtorRva, kSpeechParamsCtorSignature, sizeof(kSpeechParamsCtorSignature)) ||
            !GuardedCompare(base + kSpeechParamsDtorRva, kSpeechParamsDtorSignature, sizeof(kSpeechParamsDtorSignature)) ||
            !GuardedCompare(base + kAnimalVocalRva, kAnimalVocalSignature, sizeof(kAnimalVocalSignature)))
        {
            error = "signature_mismatch";
            return false;
        }
        const std::uint64_t speech = SpeechEntity(pedEntity);
        std::uint64_t vtable = 0;
        std::uint64_t slot = 0;
        if (!speech || !ReadU64(speech, vtable))
        {
            error = "no_speech_entity";
            return false;
        }
        // The decision is the pure check; a refusal names the RVA it found.
        const std::uint64_t module = reinterpret_cast<std::uint64_t>(base);
        const std::uint64_t vtableRva = vtable >= module ? vtable - module : ~0ull;
        if (vtable == reinterpret_cast<std::uint64_t>(base + kAnimalSpeechVtableRva))
            ReadU64(vtable + kAnimalVocalSlot, slot);
        const std::uint64_t slotRva = slot >= module ? slot - module : ~0ull;
        const replay::speech::AnimalVocalEntry entry = replay::speech::CheckAnimalVocalEntry(vtableRva, slotRva);
        if (entry != replay::speech::AnimalVocalEntry::Ok)
        {
            char text[64]{};
            std::snprintf(text, sizeof(text), "%s:vt_%llx",
                entry == replay::speech::AnimalVocalEntry::NotAnimalSpeech ? "not_animal_speech" : "slot_other",
                static_cast<unsigned long long>(vtableRva));
            error = text;
            return false;
        }
        alignas(16) std::uint8_t params[0x200] = {};
        std::uint32_t code = 0;
        if (!GuardedAnimalVocal(reinterpret_cast<AnimalVocalFn>(base + kAnimalVocalRva),
                reinterpret_cast<SpeechParamsFn>(base + kSpeechParamsCtorRva),
                reinterpret_cast<SpeechParamsFn>(base + kSpeechParamsDtorRva),
                reinterpret_cast<std::uint8_t*>(speech), params, contextPartialHash, voiceHash, variation, result, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        error = "result_" + std::to_string(result);
        return true;
    }

    namespace
    {
        bool FindPedLine(void* pedEntity, std::uint64_t line);

        bool GuardedReadPlayedLine(std::uint8_t* line, PlayedLine& played, std::uint32_t& code)
        {
            __try
            {
                ReadPlayedLine(line, played);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    // A stopped line read at its address. The line vtable comes from the line
    // ctor's own bytes (0x2F91F90, its only caller the line allocator
    // 0x2F96284): lea rax, [rip + disp] at +0x14, then mov [rcx], rax. Plain
    // guarded reads, script thread only.
    namespace
    {
        constexpr std::uintptr_t kSpeechLineCtorRva = 0x2F91F90;
        constexpr std::uint8_t kSpeechLineCtorSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20,
            0xBE, 0x02, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x05, 0x25, 0xB6, 0x86, 0x00, 0x48, 0x89, 0x01};
        constexpr std::size_t kSpeechLineCtorLeaDisp = 0x17;
        constexpr std::uintptr_t kSpeechLineVtableRva = 0x37FD5D0;

        std::uint64_t SpeechLineVtable()
        {
            static std::uint64_t vtable = 0;
            static bool checked = false;
            if (checked)
                return vtable;
            checked = true;
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            if (!GuardedCompare(base + kSpeechLineCtorRva, kSpeechLineCtorSignature, sizeof(kSpeechLineCtorSignature)))
                return vtable;
            std::uint8_t* target = RipTarget(base + kSpeechLineCtorRva, kSpeechLineCtorLeaDisp);
            if (target == base + kSpeechLineVtableRva)
                vtable = reinterpret_cast<std::uint64_t>(target);
            return vtable;
        }
    }

    bool ReadStoppedLine(std::uint64_t line, StoppedLineView& out)
    {
        out = StoppedLineView{};
        const std::uint64_t vtable = SpeechLineVtable();
        std::uint64_t first = 0;
        if (!line || !vtable || !ReadU64(line, first))
            return false;
        out.read = true;
        out.liveVtable = first == vtable;
        if (!out.liveVtable)
            return true;
        std::uint64_t data = 0;
        if (!ReadBytes(line + 0x234, &out.state, sizeof(out.state)) || !ReadU64(line + 0x1D8, data) || !data ||
            !ReadBytes(data + 0x110, &out.voice, 4) || !ReadBytes(data + 0x114, &out.context, 4) ||
            !ReadBytes(data + 0x118, &out.variation, 4))
        {
            out.read = false;
            return false;
        }
        return true;
    }

    // The identity of a line still in the ped's line list.
    bool ReadSpeechLineIdentity(void* pedEntity, std::uint64_t line, PlayedLine& out)
    {
        out = PlayedLine{};
        if (!line || !FindPedLine(pedEntity, line))
            return false;
        std::uint32_t code = 0;
        if (!GuardedReadPlayedLine(reinterpret_cast<std::uint8_t*>(line), out, code))
        {
            out = PlayedLine{};
            return false;
        }
        return out.identityRead;
    }

    namespace
    {
        // The line is one of this ped's lines (guarded list walk).
        bool FindPedLine(void* pedEntity, std::uint64_t line)
        {
            const std::uint64_t speech = SpeechEntity(pedEntity);
            std::uint64_t list = 0;
            std::uint16_t count = 0;
            if (!speech || !line || !ReadU64(speech + 0x78, list) || !list ||
                !ReadBytes(speech + 0x80, &count, sizeof(count)) || count > 64)
            {
                return false;
            }
            for (std::uint16_t slot = 0; slot < count; ++slot)
            {
                std::uint64_t entry = 0;
                if (ReadU64(list + slot * 8u, entry) && entry == line)
                    return true;
            }
            return false;
        }

        bool GuardedStopLine(SpeechLineStopFn stop, void* line, std::uint32_t reason, std::uint32_t& code)
        {
            __try
            {
                stop(line, reason);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool SpeechLineState(void* pedEntity, std::uint64_t line, std::int32_t& state)
    {
        state = -1;
        return FindPedLine(pedEntity, line) && ReadBytes(line + 0x234, &state, sizeof(state));
    }

    bool StopSpeechLine(void* pedEntity, std::uint64_t line, std::uint32_t reason, std::string& error)
    {
        const SpeechLineStopFn stop = LineStopFunction();
        if (!stop)
        {
            error = "stop_signature_mismatch";
            return false;
        }
        std::int32_t state = -1;
        if (!SpeechLineState(pedEntity, line, state))
        {
            error = "line_gone";
            return false;
        }
        if (state == 6)
        {
            error = "already_stopped";
            return false;
        }
        std::uint32_t code = 0;
        if (!GuardedStopLine(stop, reinterpret_cast<void*>(line), reason, code))
        {
            char text[32]{};
            std::snprintf(text, sizeof(text), "exception_%08X", code);
            error = text;
            return false;
        }
        error = "stopped_state_" + std::to_string(state);
        return true;
    }

    void CollectFires(std::uint64_t fromUs, std::uint64_t toUs, std::vector<FireCall>& out)
    {
        Collect(g_fires, kFireRing, g_nextFire, fromUs, toUs, out);
    }

    namespace
    {
        bool GuardedEnqueue(FireEnqueueFn fn, const std::uint8_t* info, std::uint32_t& code)
        {
            __try
            {
                fn(info);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool EnqueueFire(const std::uint8_t info[kFireInfoBytes], std::string& error)
    {
        // Through the trampoline when the observer hooked it, so the replayed
        // event is not recorded again.
        FireEnqueueFn fn = g_originalFire.load(std::memory_order_acquire);
        if (!fn)
        {
            if (!g_fireDirect)
            {
                auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
                if (!GuardedCompare(base + kFireEnqueueRva, kFireEnqueueSignature, sizeof(kFireEnqueueSignature)))
                {
                    error = "fire_enqueue_signature_mismatch";
                    return false;
                }
                g_fireDirect = reinterpret_cast<FireEnqueueFn>(base + kFireEnqueueRva);
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

    // The player's weapon settings for a puppet's weapon (see the header).
    // 0x3120F54, the engine's own set-up, is not usable on the puppet: its
    // owner lookup (+0x1C8 -> +0xF0 -> vfunc +0x58) does not give the puppet
    // ped. So the same lookup it ends in is called here with the player
    // variant. Settings by weapon hash and variant (type 0x37 is the weapon
    // audio item, 0x15 the settings, both via 0x252B5AC):
    //   hash-1 > -3 -> 0; item = find(hash, 0x37); variant ? item field
    //   `variant` (0x2EF1E70) : [item+4]; settings = find(that, 0x15)
    namespace
    {
        constexpr std::uintptr_t kWeaponSettingsLookupRva = 0x2EF1D04;
        constexpr std::uint8_t kWeaponSettingsLookupSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8D, 0x41, 0xFF, 0x8B, 0xFA, 0x83,
            0xF8, 0xFD, 0x77, 0x50, 0x8B, 0xD1, 0x41, 0xB0, 0x37, 0x48, 0x8D, 0x0D, 0x2C, 0x57, 0xC9, 0x02,
            0xE8, 0x83, 0x98, 0x63, 0xFF, 0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x74, 0x37, 0x85, 0xFF, 0x74,
            0x1F, 0x44, 0x8B, 0xC7, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0x48, 0x8B, 0xD0, 0xE8, 0x2B, 0x01, 0x00,
            0x00, 0x8B, 0x10, 0x44, 0x8D, 0x42, 0x01, 0x41, 0xF7, 0xC0, 0xFE, 0xFF, 0xFF, 0xFF, 0x75, 0x03,
            0x8B, 0x53, 0x04, 0x41, 0xB0, 0x15, 0x48, 0x8D, 0x0D, 0xEF, 0x56, 0xC9, 0x02, 0xE8, 0x46, 0x98,
            0x63, 0xFF, 0xEB, 0x02, 0x33, 0xC0, 0x48, 0x8B, 0x5C, 0x24, 0x38, 0x48, 0x83, 0xC4, 0x20, 0x5F,
            0xC3};
        constexpr std::uint32_t kPlayerWeaponSettingsVariant = 0x6F0783F5; // 0x3110424
        constexpr std::size_t kWeaponAudioOffset = 0x100;     // CWeapon -> its weapon audio
        constexpr std::size_t kWeaponAudioOwner = 0x1C8;      // set up: the weapon the audio belongs to
        constexpr std::size_t kWeaponAudioHash = 0x1E0;       // weapon audio item hash
        constexpr std::size_t kWeaponAudioSettings = 0x1F8;   // cached settings (0x311971C)
        using WeaponSettingsLookupFn = std::uint64_t(__fastcall*)(std::uint32_t, std::uint32_t);
        WeaponSettingsLookupFn g_weaponSettingsLookup = nullptr;

        bool GuardedSettingsLookup(WeaponSettingsLookupFn fn, std::uint32_t hash, std::uint32_t variant,
            std::uint64_t& out, std::uint32_t& code)
        {
            __try
            {
                out = fn(hash, variant);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
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
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            if (!GuardedCompare(base + kWeaponSettingsLookupRva, kWeaponSettingsLookupSignature,
                    sizeof(kWeaponSettingsLookupSignature)))
            {
                error = "weapon_settings_lookup_signature_mismatch";
                return false;
            }
            g_weaponSettingsLookup = reinterpret_cast<WeaponSettingsLookupFn>(base + kWeaponSettingsLookupRva);
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
        // e.g. by the camera branch of 0x311971C, is left alone).
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

    namespace
    {
        // Recorded init params over constructed ones: the value fields only.
        // +0xC8 tracker, +0xD0 wave slot, +0xD8 category, +0xE0 environment
        // group (ref-counted or engine-owned) and the u16 handle at +0x12C
        // stay as the constructor left them. The tail [0x12E, 0x140) holds
        // value fields with non-zero defaults (ctor 0x487E34: +0x130 dword -1,
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

        bool GuardedSoundRequest(SoundRequestFn play, SoundParamsCtorFn construct, SoundParamsReleaseFn release,
            std::uint64_t audio, std::uint64_t soundRef, std::uint64_t owner, const std::uint8_t* recorded,
            const std::uint64_t flags[4], bool& queued, std::uint32_t& code)
        {
            __try
            {
                alignas(16) std::uint8_t params[kSoundParamsBytes + 0x40] = {};
                std::uint64_t paramsArg = 0;
                if (recorded)
                {
                    construct(params);
                    CopyRecordedSoundParams(params, recorded);
                    paramsArg = reinterpret_cast<std::uint64_t>(params);
                }
                queued = play(audio, soundRef, owner, paramsArg, flags[0], flags[1], flags[2], flags[3]);
                if (recorded)
                    release(params + 0x12C);
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        // The constructor is observed: its prologue is patched, so the
        // detour's trampoline (the original code) is used when installed.
        static SoundParamsReleaseFn release = nullptr;
        SoundParamsCtorFn construct = g_originalCtor.load(std::memory_order_acquire);
        if (!construct)
        {
            if (!GuardedCompare(base + kSoundParamsCtorRva, kSoundParamsCtorSignature, sizeof(kSoundParamsCtorSignature)))
            {
                error = "sound_params_signature_mismatch";
                return false;
            }
            construct = reinterpret_cast<SoundParamsCtorFn>(base + kSoundParamsCtorRva);
        }
        if (!release)
        {
            if (!GuardedCompare(base + kSoundParamsReleaseRva, kSoundParamsReleaseSignature, sizeof(kSoundParamsReleaseSignature)))
            {
                error = "sound_params_release_signature_mismatch";
                return false;
            }
            release = reinterpret_cast<SoundParamsReleaseFn>(base + kSoundParamsReleaseRva);
        }
        SoundRequestFn play = g_originalSoundRequest.load(std::memory_order_acquire);
        if (!play)
        {
            if (!GuardedCompare(base + kSoundRequestRva, kSoundRequestSignature, sizeof(kSoundRequestSignature)))
            {
                error = "sound_request_signature_mismatch";
                return false;
            }
            play = reinterpret_cast<SoundRequestFn>(base + kSoundRequestRva);
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
        bool GuardedDirectSound(CreateAndPlayFn play, SoundParamsCtorFn construct, SoundParamsReleaseFn release,
            std::uint64_t audio, std::uint64_t soundRef, const std::uint8_t* recorded, std::uint64_t tracker,
            std::uint64_t envGroup, bool& queued, std::uint32_t& code)
        {
            __try
            {
                alignas(16) std::uint8_t params[kSoundParamsBytes + 0x40] = {};
                construct(params);
                if (recorded)
                    CopyRecordedSoundParams(params, recorded);
                // The tracker is reference-counted by the core itself.
                std::memcpy(params + 0xC8, &tracker, sizeof(tracker));
                // The environment group, as the game's own weapon rattle
                // 0x31906D4 puts its holder's at +0xE0 (0x31907EA) before the
                // same public entry; the creator counts the use.
                if (envGroup)
                    std::memcpy(params + 0xE0, &envGroup, sizeof(envGroup));
                queued = play(audio, soundRef, reinterpret_cast<std::uint64_t>(params));
                release(params + 0x12C);
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

    bool TrustedImageAudio(std::uint64_t address, std::string& status)
    {
        const std::uint64_t module = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
        std::uint32_t vtableRva = 0;
        char rva[24]{};
        std::snprintf(rva, sizeof(rva), "(0x%llx)", static_cast<unsigned long long>(address > module ? address - module : address));
        if (address <= module || !census::KnownImageAudio(address - module, vtableRva))
        {
            status = std::string("unknown_image_audio") + rva; // the RVA: evidence to extend kImageAudio
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        static CreateAndPlayFn play = nullptr;
        static SoundParamsReleaseFn release = nullptr;
        if (!play)
        {
            if (!GuardedCompare(base + kCreateAndPlayPublicRva, kCreateAndPlayPublicSignature,
                    sizeof(kCreateAndPlayPublicSignature)) ||
                !GuardedCompare(base + kSoundParamsReleaseRva, kSoundParamsReleaseSignature,
                    sizeof(kSoundParamsReleaseSignature)))
            {
                error = "direct_sound_signature_mismatch";
                return false;
            }
            play = reinterpret_cast<CreateAndPlayFn>(base + kCreateAndPlayPublicRva);
            release = reinterpret_cast<SoundParamsReleaseFn>(base + kSoundParamsReleaseRva);
        }
        SoundParamsCtorFn construct = g_originalCtor.load(std::memory_order_acquire);
        if (!construct)
        {
            if (!GuardedCompare(base + kSoundParamsCtorRva, kSoundParamsCtorSignature, sizeof(kSoundParamsCtorSignature)))
            {
                error = "sound_params_signature_mismatch";
                return false;
            }
            construct = reinterpret_cast<SoundParamsCtorFn>(base + kSoundParamsCtorRva);
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
            // Why (0x2588950 / 0x2589A40): the audio entity's state (+0x14,
            // 0 refuses) and its registration (+0x10, 0xFFFF refuses).
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
        return reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr)) + kStaticCollisionAudioRva;
    }

    namespace
    {
        // Requested settings of a sound (the lookup StopAndForget inlines);
        // globals taken from its verified bytes. 0 when the sound has none.
        std::uint64_t RequestedSettings(std::uint64_t sound)
        {
            static std::uint64_t poolTable = 0;
            static std::uint64_t poolStride = 0;
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            if (!poolTable)
            {
                std::uint8_t* stop = base + kStopAndForgetRva;
                // The displacements sit past the hook patch: imul ecx,[rip+d]
                // at +0x1D (d at +0x20) and mov rax,[rip+d] at +0x2B (d at
                // +0x2E). They must land on the expected globals.
                std::int32_t strideDisp = 0;
                std::int32_t tableDisp = 0;
                if (!GuardedRead(&strideDisp, stop + 0x20, 4) || !GuardedRead(&tableDisp, stop + 0x2E, 4))
                    return 0;
                const std::uint64_t stride = reinterpret_cast<std::uint64_t>(stop + 0x24 + strideDisp);
                const std::uint64_t table = reinterpret_cast<std::uint64_t>(stop + 0x32 + tableDisp);
                if (stride != reinterpret_cast<std::uint64_t>(base + 0x53588B4) ||
                    table != reinterpret_cast<std::uint64_t>(base + 0x53588D8))
                {
                    return 0;
                }
                poolStride = stride;
                poolTable = table;
            }
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
        // slot the engine nulls. The settings block is allocated by 0x251BB1C
        // from the pool 0x251BB94 (which zeroes the block) and built by
        // 0x251972C with `mov [block], sound`; the release 0x2573340 frees the
        // block (0x25276C4, index free list outside the block) and writes
        // 0xFFFF to sound+0xD6. A top-level sound's vtable slot 1 is S38
        // (0x25735EC). A freed or reused sound fails one of the three: not a
        // sound, index 0xFFFF or garbage, or the block names another sound.
        // Reads only, through the range filter (no fault, so no other mod's
        // vectored handler runs).
        constexpr std::uintptr_t kSoundVtableSlot1Rva = 0x25735EC; // S38
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
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            if (!sound || (sound & 7u) != 0 || !GuardedRead(&vtable, reinterpret_cast<const void*>(sound), 8) ||
                !GuardedRead(&slot1, reinterpret_cast<const void*>(vtable + 8), 8) ||
                slot1 != reinterpret_cast<std::uint64_t>(base + kSoundVtableSlot1Rva))
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
    }

    bool ReadSoundVariable(std::uint64_t sound, std::uint32_t hash, float& value)
    {
        static GetVariableFn get = nullptr;
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!get)
        {
            if (!GuardedCompare(base + kGetVariableRva, kGetVariableSignature, sizeof(kGetVariableSignature)))
                return false;
            get = reinterpret_cast<GetVariableFn>(base + kGetVariableRva);
        }
        if (!sound || !PortSettings(sound, kLinkVariable))
            return false;
        struct Context { GetVariableFn get; std::uint64_t sound; std::uint32_t hash; float* value; bool ok; };
        Context context{get, sound, hash, &value, false};
        std::uint32_t code = 0;
        return GuardedCall([](void* raw) {
            auto* c = static_cast<Context*>(raw);
            c->ok = c->get(c->sound, c->hash, c->value);
        }, &context, code) && context.ok;
    }

    bool WriteSoundVariable(std::uint64_t sound, std::uint32_t hash, float value)
    {
        return std::strcmp(WriteSoundVariableWhy(sound, hash, value), "ok") == 0;
    }

    // Why S38 refuses (the lookup it runs, read with guarded reads after the
    // signatures of 0x25736B4 and 0x25745A8 are checked).
    const char* SoundVariableRefusal(std::uint64_t sound, std::uint32_t hash, float value)
    {
        if (!std::isfinite(value))
            return "bad_value";
        static int checked = 0; // 0 unknown, 1 ok, -1 mismatch
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!checked)
            checked = GuardedCompare(base + kVariableFindRva, kVariableFindSignature, sizeof(kVariableFindSignature)) &&
                    GuardedCompare(base + kRootSoundRva, kRootSoundSignature, sizeof(kRootSoundSignature))
                ? 1
                : -1;
        if (checked < 0)
            return "unchecked";
        std::uint64_t table = 0;
        std::uint32_t stride = 0;
        if (!ReadU64(reinterpret_cast<std::uint64_t>(base + kSoundPoolTableRva), table) || !table ||
            !GuardedRead(&stride, base + kSoundSlotStrideRva, 4) || !stride)
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
        static SetPositionFn set = nullptr;
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        // With SoundFxProbe=1 the read probe hooked this port (the check below
        // would see the hook's jump): the mod calls the probe's trampoline,
        // which is the engine's function, uncounted.
        if (!set)
            set = reinterpret_cast<SetPositionFn>(replay::soundfx::OriginalFor(kSetPositionRva));
        if (!set)
        {
            std::uint8_t head[2] = {};
            if (!GuardedRead(head, base + kSetPositionRva, 2) || head[0] != 0x8B || head[1] != 0x05 ||
                !GuardedCompare(base + kSetPositionRva + 6, kSetPositionTail, sizeof(kSetPositionTail)))
            {
                return false;
            }
            set = reinterpret_cast<SetPositionFn>(base + kSetPositionRva);
        }
        const std::uint64_t settings = PortSettings(sound, kLinkAdjust);
        if (!settings)
            return false;
        alignas(16) float vector[4] = {position[0], position[1], position[2], 0.0f};
        struct Context { SetPositionFn set; std::uint64_t settings; const float* vector; };
        Context context{set, settings, vector};
        std::uint32_t code = 0;
        return GuardedCall([](void* raw) {
            auto* c = static_cast<Context*>(raw);
            c->set(c->settings, c->vector);
        }, &context, code);
    }

    bool StartPersistentSound(std::uint64_t audio, std::uint64_t soundRef, const std::uint8_t* params,
        std::uint64_t* slot, std::string& error)
    {
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        static PrepareAndPlayFn play = nullptr;
        static SoundParamsReleaseFn release = nullptr;
        if (!play)
        {
            // A port the read probe hooked (SoundFxProbe=1) is called through
            // the probe's trampoline; its bytes start with the hook's jump.
            void* probed = replay::soundfx::OriginalFor(kPrepareAndPlayRva);
            if ((!probed &&
                    !GuardedCompare(base + kPrepareAndPlayRva, kPrepareAndPlaySignature, sizeof(kPrepareAndPlaySignature))) ||
                !GuardedCompare(base + kSoundParamsReleaseRva, kSoundParamsReleaseSignature, sizeof(kSoundParamsReleaseSignature)))
            {
                error = "persistent_signature_mismatch";
                return false;
            }
            play = reinterpret_cast<PrepareAndPlayFn>(probed ? probed : base + kPrepareAndPlayRva);
            release = reinterpret_cast<SoundParamsReleaseFn>(base + kSoundParamsReleaseRva);
        }
        const PersistentCreateFn create = g_originalPersistentCreate.load(std::memory_order_acquire);
        const SoundParamsCtorFn construct = g_originalCtor.load(std::memory_order_acquire);
        if (!create || !construct)
        {
            error = "observer_not_installed";
            return false;
        }
        StopPersistentSound(slot);
        struct Context
        {
            PersistentCreateFn create; SoundParamsCtorFn construct; SoundParamsReleaseFn release;
            PrepareAndPlayFn play; std::uint64_t audio; std::uint64_t soundRef; const std::uint8_t* recorded;
            std::uint64_t* slot;
        };
        Context context{create, construct, release, play, audio, soundRef, params, slot};
        std::uint32_t code = 0;
        const bool ok = GuardedCall([](void* raw) {
            auto* c = static_cast<Context*>(raw);
            alignas(16) std::uint8_t params[kSoundParamsBytes + 0x40] = {};
            c->construct(params);
            if (c->recorded)
                CopyRecordedSoundParams(params, c->recorded);
            // Descriptor: the metadata ref at +0xC (0x14 bytes).
            alignas(16) std::uint8_t descriptor[0x20] = {};
            std::memcpy(descriptor + 0xC, &c->soundRef, sizeof(c->soundRef));
            c->create(c->audio, reinterpret_cast<std::uint64_t>(descriptor),
                reinterpret_cast<std::uint64_t>(c->slot), reinterpret_cast<std::uint64_t>(params));
            c->release(params + 0x12C);
            if (*c->slot)
                c->play(*c->slot, 0, 0, 0, 0);
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        static PrepareAndPlayFn play = nullptr;
        static SoundParamsReleaseFn release = nullptr;
        if (!play)
        {
            // A port the read probe hooked (SoundFxProbe=1) is called through
            // the probe's trampoline; its bytes start with the hook's jump.
            void* probed = replay::soundfx::OriginalFor(kPrepareAndPlayRva);
            if ((!probed &&
                    !GuardedCompare(base + kPrepareAndPlayRva, kPrepareAndPlaySignature, sizeof(kPrepareAndPlaySignature))) ||
                !GuardedCompare(base + kSoundParamsReleaseRva, kSoundParamsReleaseSignature, sizeof(kSoundParamsReleaseSignature)))
            {
                error = "persistent_signature_mismatch";
                return false;
            }
            play = reinterpret_cast<PrepareAndPlayFn>(probed ? probed : base + kPrepareAndPlayRva);
            release = reinterpret_cast<SoundParamsReleaseFn>(base + kSoundParamsReleaseRva);
        }
        // Through the trampolines: the replayed create is not recorded again.
        const PersistentByRefFn byRef = g_originalByRef.load(std::memory_order_acquire);
        const PersistentByHashFn byHashFn = g_originalByHash.load(std::memory_order_acquire);
        const SoundParamsCtorFn construct = g_originalCtor.load(std::memory_order_acquire);
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
            PersistentByRefFn byRef; PersistentByHashFn byHash; bool useHash; SoundParamsCtorFn construct;
            SoundParamsReleaseFn release; PrepareAndPlayFn play; std::uint64_t audio; std::uint64_t soundRef;
            const std::uint8_t* recorded; std::uint64_t* slot;
        };
        Context context{byRef, byHashFn, byHash, construct, release, play, audio, soundRef, params, slot};
        std::uint32_t code = 0;
        const bool ok = GuardedCall([](void* raw) {
            auto* c = static_cast<Context*>(raw);
            alignas(16) std::uint8_t params[kSoundParamsBytes + 0x40] = {};
            c->construct(params);
            if (c->recorded)
                CopyRecordedSoundParams(params, c->recorded); // tracker [0xC8, 0xE8) left clear
            if (c->useHash)
                c->byHash(c->audio, static_cast<std::uint32_t>(c->soundRef), reinterpret_cast<std::uint64_t>(c->slot),
                    reinterpret_cast<std::uint64_t>(params));
            else
                c->byRef(c->audio, c->soundRef, reinterpret_cast<std::uint64_t>(c->slot), reinterpret_cast<std::uint64_t>(params));
            c->release(params + 0x12C);
            if (*c->slot)
                c->play(*c->slot, 0, 0, 0, 0);
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
        StopAndForgetFn stop = g_originalStopAndForget.load(std::memory_order_acquire);
        if (!stop)
            return;
        struct Context { StopAndForgetFn stop; std::uint64_t sound; };
        Context context{stop, *slot};
        std::uint32_t code = 0;
        GuardedCall([](void* raw) {
            auto* c = static_cast<Context*>(raw);
            c->stop(c->sound, 0);
        }, &context, code);
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
        bool GuardedMelee(MeleeImpactFn play, void* melee, void* info, std::uint32_t& code)
        {
            __try
            {
                play(melee, info);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The action result lookup (see kActionResultLookupSiteRva): the
        // site's bytes with the manager displacement masked, then its call
        // target must be the documented RVA with the documented prologue.
        ActionResultLookupFn g_actionResultLookup = nullptr;
        std::uint8_t* g_actionResultManager = nullptr;
        bool g_actionResultChecked = false;
        std::string g_actionResultCheckError;

        bool EnsureActionResultLookup(std::string& error)
        {
            if (g_actionResultChecked)
            {
                error = g_actionResultCheckError;
                return g_actionResultLookup != nullptr;
            }
            g_actionResultChecked = true;
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            std::uint8_t* site = base + kActionResultLookupSiteRva;
            if (!GuardedCompare(site, kActionResultLookupSiteHead, sizeof(kActionResultLookupSiteHead)) ||
                !GuardedCompare(site + kActionResultManagerDisp + 4, kActionResultLookupSiteTail,
                    sizeof(kActionResultLookupSiteTail)))
            {
                g_actionResultCheckError = error = "action_result_site_mismatch";
                return false;
            }
            std::uint8_t* lookup = RipTarget(site, kActionResultLookupCall + 1);
            if (lookup != base + kActionResultLookupRva ||
                !GuardedCompare(lookup, kActionResultLookupSignature, sizeof(kActionResultLookupSignature)))
            {
                g_actionResultCheckError = error = "action_result_lookup_mismatch";
                return false;
            }
            g_actionResultManager = RipTarget(site, kActionResultManagerDisp);
            g_actionResultLookup = reinterpret_cast<ActionResultLookupFn>(lookup);
            return true;
        }

        bool GuardedActionResultLookup(void* manager, std::uint32_t hash, void*& result, std::uint32_t& code)
        {
            __try
            {
                result = g_actionResultLookup(manager, hash);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        MeleeImpactFn play = g_originalMeleeImpact.load(std::memory_order_acquire);
        if (!play)
        {
            if (!GuardedCompare(base + kMeleeImpactRva, kMeleeImpactSignature, sizeof(kMeleeImpactSignature)))
            {
                error = "melee_signature_mismatch";
                return false;
            }
            play = reinterpret_cast<MeleeImpactFn>(base + kMeleeImpactRva);
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
        bool GuardedExplosionPlay(ExplosionPlayFn play, void* manager, std::uint32_t ref, const float* position,
            const ExplosionAudioCall& call, std::uint32_t& code)
        {
            __try
            {
                play(manager, ref, position, call.underwater, call.b31, call.interior, call.b32);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedTagRef(ExplosionTagRefFn fn, std::int32_t tag, std::uint32_t& ref, std::uint32_t& code)
        {
            __try
            {
                std::uint32_t out = 0;
                ref = *fn(&out, tag);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool PlayExplosionAudio(const ExplosionAudioCall& call, std::string& error)
    {
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        static int checked = 0; // 1 ok, -1 mismatch
        if (!checked)
            checked = GuardedCompare(base + kExplosionPlayRva, kExplosionPlaySignature, sizeof(kExplosionPlaySignature)) &&
                    GuardedCompare(base + kExplosionTagRefRva, kExplosionTagRefSignature, sizeof(kExplosionTagRefSignature))
                ? 1 : -1;
        if (checked < 0)
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
            if (!GuardedTagRef(reinterpret_cast<ExplosionTagRefFn>(base + kExplosionTagRefRva), call.tag, ref, code))
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
        const bool ok = GuardedExplosionPlay(reinterpret_cast<ExplosionPlayFn>(base + kExplosionPlayRva),
            base + kExplosionManagerRva, ref, position, call, code);
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
    }

    namespace
    {
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

    void SetFootMaterialReplay(bool enabled) { g_footMaterialReplay.store(enabled, std::memory_order_relaxed); }
    void SetFootWetnessReplay(bool enabled) { g_footWetnessReplay.store(enabled, std::memory_order_relaxed); }
    bool FootWetnessReplay() { return g_footWetnessReplay.load(std::memory_order_relaxed); }
    bool FootMaterialReplay() { return g_footMaterialReplay.load(std::memory_order_relaxed); }
    void SetFootMixReplay(bool enabled) { g_footMixReplay.store(enabled, std::memory_order_relaxed); }
    void SetFootDriveReplay(bool base, bool impact)
    {
        g_footBaseReplay.store(base, std::memory_order_relaxed);
        g_footImpactReplay.store(impact, std::memory_order_relaxed);
    }
    bool FootMixReplay() { return g_footMixReplay.load(std::memory_order_relaxed); }

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

    void SetPuppetPersistentByEntry(bool enabled)
    {
        g_persistentByEntry.store(enabled, std::memory_order_relaxed);
    }

    bool PuppetPersistentByEntry()
    {
        return g_persistentByEntry.load(std::memory_order_relaxed);
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
    void SetBlockPacketLives(const replay::soundowner::PacketLives* lives, bool blockAll, bool stepSetGate)
    {
        g_blockAll.store(blockAll, std::memory_order_relaxed);
        g_stepSetGate.store(stepSetGate, std::memory_order_relaxed);
        g_blockLives.store(lives, std::memory_order_release);
    }
    void SetBlockPlayheadMs(std::uint32_t playheadMs) { g_blockPlayheadMs.store(playheadMs, std::memory_order_relaxed); }
    bool BlockPuppetSounds() { return g_blockPuppetSounds.load(std::memory_order_acquire); }
    std::uint64_t RefusedUnmadeSets() { return g_refusedUnmadeSets.load(std::memory_order_relaxed); }
    void SetPuppetUnrecordedPersistent(bool enabled) { g_refuseUnrecordedPersistent.store(enabled, std::memory_order_relaxed); }
    bool PuppetUnrecordedPersistent() { return g_refuseUnrecordedPersistent.load(std::memory_order_relaxed); }
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
        std::snprintf(line, sizeof(line), "PUPPET_UNRECORDED_PERSISTENT,enabled=%d,refused=%llu,kept_by_esnd=%llu,samples=",
            g_refuseUnrecordedPersistent.load(std::memory_order_relaxed) ? 1 : 0,
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
    // come with one (0x25889A8 passes its caller's slot through 0x2588F20 and
    // 0x2516774 to the wrapper 0x25167D0, which zeroes it when the creator
    // returns 0; 0x25889A8 then plays nothing).
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

    std::uint64_t FootstepAudio(void* pedEntity)
    {
        const std::uint64_t holder = PedAudioHolder(pedEntity);
        return holder ? holder + kFootAudioOffset : 0;
    }

    namespace
    {
        bool GuardedFootstep(FootstepFn play, std::uint8_t* footAudio, const FootstepCall& step,
            std::uint32_t& code)
        {
            __try
            {
                play(footAudio, step.eventId, step.arg3, step.foot, step.arg5, step.flag, step.arg7, step.arg8);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    namespace
    {
        // Guarded write on the puppet's own foot audio (vtable checked) of the
        // container the engine's own lookup returns for the recorded ref
        // (0x252B194, its signature checked, as IndirectRefState). `stats`
        // lets the secondary steps (PlayFootEvent) keep their own counters.
        void SetPuppetFootMaterial(std::uint8_t* footAudio, const FootstepCall& step,
            FootMaterialStats& stats = g_footMaterialStats)
        {
            ++stats.requested;
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            std::uint64_t vtable = 0;
            if (step.foot >= footmaterial::kFeet || !GuardedRead(&vtable, footAudio, 8) ||
                vtable != reinterpret_cast<std::uint64_t>(base) + kFootVtableRva)
            {
                ++stats.badFoot;
                return;
            }
            static int checked = -1;
            if (checked < 0)
                checked = GuardedCompare(base + kRefContainerRva, kRefContainerSignature, sizeof(kRefContainerSignature)) ? 1 : 0;
            if (!checked)
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
                if (!GuardedRefContainer(reinterpret_cast<RefContainerFn>(base + kRefContainerRva), step.materialRef, found))
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
    }

    namespace
    {
        // foot_material.h FootMix: guarded write of the recorded per-foot
        // volume and filters on the puppet's own foot audio (vtable checked,
        // as SetPuppetFootWetness), kept in the table for the movement update
        // entry.
        void SetPuppetFootMix(std::uint8_t* footAudio, const FootstepCall& step)
        {
            FootMaterialStats& stats = g_footMaterialStats;
            ++stats.mixRequested;
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            std::uint64_t vtable = 0;
            if (step.foot >= footmaterial::kFeet || !GuardedRead(&vtable, footAudio, 8) ||
                vtable != reinterpret_cast<std::uint64_t>(base) + kFootVtableRva)
            {
                ++stats.mixBadFoot;
                return;
            }
            footmaterial::FootMix mix = g_footMixReplay.load(std::memory_order_relaxed) ? step.mix : footmaterial::FootMix{};
            SetFootMixEntry(reinterpret_cast<std::uint64_t>(footAudio), step.foot, mix,
                g_footImpactReplay.load(std::memory_order_relaxed) || g_footBaseReplay.load(std::memory_order_relaxed)
                    ? step.drive : footmaterial::FootDrive{});
            if (g_footImpactReplay.load(std::memory_order_relaxed) && step.drive.impactKnown &&
                WriteFootImpact(footAudio, step.foot, step.drive.impact))
                ++stats.impactWritten;
            if (!mix.known)
                return;
            float puppetBase = 0.0f;
            if (ReadFootBase(footAudio, puppetBase) && g_footBaseReplay.load(std::memory_order_relaxed) && step.drive.baseKnown)
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
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            std::uint64_t vtable = 0;
            if (step.foot >= footmaterial::kFeet || !GuardedRead(&vtable, footAudio, 8) ||
                vtable != reinterpret_cast<std::uint64_t>(base) + kFootVtableRva)
            {
                ++stats.wetBadFoot;
                return;
            }
            SetFootWetnessEntry(reinterpret_cast<std::uint64_t>(footAudio), step.foot, step.wetness);
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
    }

    namespace
    {
        // foot_material.h kLayerBase: guarded write of the recorded ground
        // layer weights on the puppet's own foot audio (vtable checked, as
        // SetPuppetFootWetness), kept in the table for the movement update
        // entry, where the step reads them.
        void SetPuppetFootLayers(std::uint8_t* footAudio, const FootstepCall& step)
        {
            FootMaterialStats& stats = g_footMaterialStats;
            ++stats.layerRequested;
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            std::uint64_t vtable = 0;
            if (step.foot >= footmaterial::kFeet || !GuardedRead(&vtable, footAudio, 8) ||
                vtable != reinterpret_cast<std::uint64_t>(base) + kFootVtableRva)
            {
                ++stats.layerBadFoot;
                return;
            }
            SetFootLayersEntry(reinterpret_cast<std::uint64_t>(footAudio), step.foot, step.layers);
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        FootstepFn play = g_originalFootstep.load(std::memory_order_acquire);
        if (!play)
        {
            if (!GuardedCompare(base + kFootstepRva, kFootstepSignature, sizeof(kFootstepSignature)))
            {
                error = "footstep_signature_mismatch";
                return false;
            }
            play = reinterpret_cast<FootstepFn>(base + kFootstepRva);
        }
        auto* footAudio = reinterpret_cast<std::uint8_t*>(FootstepAudio(pedEntity));
        std::uint8_t enabled = 0;
        if (!footAudio || !GuardedRead(&enabled, footAudio + 0x24, 1))
        {
            error = "no_footstep_audio";
            return false;
        }
        // The foot-down handler only queues when this is set (0x8C2B64).
        if (!enabled)
        {
            // Why (0x317A0A0): a pending component is activated only while its
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
                // (0x3196B60: active only while LOD <= max).
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
        if (g_footMaterialReplay.load(std::memory_order_relaxed) && footmaterial::ValidRef(step.materialRef))
            SetPuppetFootMaterial(footAudio, step);
        if (g_footWetnessReplay.load(std::memory_order_relaxed) && step.wetness >= 0.0f)
            SetPuppetFootWetness(footAudio, step);
        if ((g_footMixReplay.load(std::memory_order_relaxed) && step.mix.known) ||
            (g_footImpactReplay.load(std::memory_order_relaxed) && step.drive.impactKnown))
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

    // foot_event_core.h: the second queue of the foot audio, script thread
    // side.
    namespace
    {
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
        bool GuardedFootEvent(FootEventFn play, std::uint8_t* footAudio, std::int32_t type, std::uint8_t foot,
            std::uint32_t& code)
        {
            __try
            {
                play(footAudio, type, foot, 0, 1);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        FootEventFn play = g_originalFootEvent.load(std::memory_order_acquire);
        if (!play)
        {
            // Not detoured (the install failed on a site): the entry itself,
            // its prologue and its store checked.
            if (!GuardedCompare(base + footevent::kEnqueueRva, footevent::kEnqueueSignature,
                    sizeof(footevent::kEnqueueSignature)) ||
                !GuardedCompare(base + footevent::kEnqueueStoreRva, footevent::kEnqueueStoreSignature,
                    sizeof(footevent::kEnqueueStoreSignature)))
                return fail(PlayStatus::Signature);
            play = reinterpret_cast<FootEventFn>(base + footevent::kEnqueueRva);
        }
        auto* footAudio = reinterpret_cast<std::uint8_t*>(FootstepAudio(pedEntity));
        std::uint64_t vtable = 0;
        if (!footAudio || !GuardedRead(&vtable, footAudio, 8) ||
            vtable != reinterpret_cast<std::uint64_t>(base) + kFootVtableRva)
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
        // The recorded container of that foot's slot (the sound 0x318D470
        // reads it when the queue plays) and the recorded rise, as
        // PlayFootstep does for a main step; the movement update entry sets
        // both again (MovementUpdateDetour, the second queue counted).
        if (g_footMaterialReplay.load(std::memory_order_relaxed) && footmaterial::ValidRef(materialRef))
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
        // The component's ped, as 0x8AABCC reads it ([C +8] & ~0xF).
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
        bool GuardedAddAnimEvent(AddAnimEventFn add, void* audio, const std::uint8_t* event, std::uint32_t& code)
        {
            __try
            {
                add(audio, event);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
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
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        // Checked once: a mismatch disables the replay of these events.
        static int verified = -1;
        if (verified < 0)
            verified = GuardedCompare(reinterpret_cast<const void*>(base + kAddAnimEventRva), kAddAnimEventSignature,
                sizeof(kAddAnimEventSignature)) ? 1 : 0;
        if (!verified)
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
        for (const std::uintptr_t rva : kPedAudioVtableRvas)
            pedVtable = pedVtable || vtable == base + rva;
        if (!pedVtable)
        {
            status = "vtable_other";
            return false;
        }
        std::uint64_t slot = 0;
        if (!ReadU64(vtable + 0xB8, slot) || slot != base + kAddAnimEventRva)
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
        if (!GuardedAddAnimEvent(reinterpret_cast<AddAnimEventFn>(base + kAddAnimEventRva),
                reinterpret_cast<void*>(audio), event, code))
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

    void SetPuppetSpeechEntities(const std::vector<std::uint64_t>& entities, const std::vector<std::uint32_t>& owners)
    {
        const std::uint32_t count = static_cast<std::uint32_t>((std::min)(entities.size(), kMaxPuppetSpeech));
        if (count < g_puppetSpeechCount.load(std::memory_order_relaxed))
            g_puppetSpeechCount.store(count, std::memory_order_release);
        for (std::uint32_t n = 0; n < count; ++n)
        {
            g_puppetSpeechOwner[n].store(n < owners.size() ? owners[n] : replay::puppetaudio::kNoOwner,
                std::memory_order_relaxed);
            g_puppetSpeech[n].store(entities[n], std::memory_order_release);
        }
        g_puppetSpeechCount.store(count, std::memory_order_release);
    }

    void SetPuppetOwnSpeechBlock(bool block) { g_puppetOwnSpeechBlock.store(block, std::memory_order_relaxed); }
    bool PuppetOwnSpeechBlock()
    {
        return g_puppetOwnSpeechBlock.load(std::memory_order_relaxed) &&
            g_ownSpeechRelease.load(std::memory_order_acquire) != nullptr;
    }
    const char* PuppetOwnSpeechError() { return g_ownSpeechError; }
    std::uint64_t PuppetOwnSpeechSeen() { return g_puppetOwnSpeechSeen.load(std::memory_order_relaxed); }
    std::uint64_t PuppetOwnSpeechRefused() { return g_puppetOwnSpeechRefused.load(std::memory_order_relaxed); }
    std::uint64_t WorldSpeechDuringPlayback() { return g_worldSpeechDuringPlayback.load(std::memory_order_relaxed); }
    std::uint64_t WorldSpeechQueuedDuringPlayback() { return g_worldSpeechQueuedDuringPlayback.load(std::memory_order_relaxed); }

    void CollectPuppetOwnSpeechSince(std::uint64_t& cursor, std::vector<PuppetOwnSpeech>& out)
    {
        CollectSince(g_ownSpeech, kOwnSpeechRing, g_nextOwnSpeech, cursor, out);
    }

    bool FindLineStop(std::uint64_t line, SpeechLineStop& out)
    {
        const std::uint64_t end = g_nextStop.load(std::memory_order_acquire);
        const std::uint64_t first = end > kStopRing ? end - kStopRing : 0;
        for (std::uint64_t index = end; index-- > first;)
        {
            SpeechLineStop value;
            if (replay::ring::TryRead(g_stops[index % kStopRing], index, value) == replay::ring::Read::Ready && value.line == line)
            {
                out = value;
                return true;
            }
        }
        return false;
    }

    void CollectSpeechStopsSince(std::uint64_t& cursor, std::vector<SpeechLineStop>& out)
    {
        CollectSince(g_stops, kStopRing, g_nextStop, cursor, out);
    }

    void CollectSpeechStartsSince(std::uint64_t& cursor, std::vector<SpeechLineStart>& out)
    {
        CollectSince(g_starts, kStartRing, g_nextStart, cursor, out);
    }

    bool SpeechStartObserved()
    {
        return g_audibleTarget != nullptr;
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

    // Player weapon sound on the main character's puppet (see the header).
    // The predicate, whole function (0x4E bytes, ends in ret):
    //   cmp byte [rcx+30h],4 / jne no ; ped only
    //   row = [0x39ECE48] + ((dword [rcx+9Ch] & 1FFFFh) - [0x39ECE40]) * 148h
    //   comp = [row+0B8h] & ~1 ; flags = byte [comp+40h]
    //   yes when !(flags & 1) && (flags & 2)
    namespace
    {
        constexpr std::uintptr_t kPlayerAudioPredicateRva = 0x2F02F8C;
        constexpr std::uint8_t kPlayerAudioPredicateSignature[] = {
            0x80, 0x79, 0x30, 0x04, 0x75, 0x45, 0x8B, 0x81, 0x9C, 0x00, 0x00, 0x00, 0x25, 0xFF, 0xFF, 0x01,
            0x00, 0x2B, 0x05, 0x9D, 0x9E, 0xAE, 0x00, 0x48, 0x69, 0xC8, 0x48, 0x01, 0x00, 0x00, 0x48, 0x8B,
            0x05, 0x97, 0x9E, 0xAE, 0x00, 0x48, 0x8B, 0x94, 0x01, 0xB8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCA,
            0x48, 0x83, 0xE1, 0xFE, 0x48, 0xF7, 0xDA, 0x48, 0x1B, 0xC0, 0x48, 0x23, 0xC1, 0x8A, 0x48, 0x40,
            0xB0, 0x01, 0x84, 0xC8, 0x75, 0x05, 0xF6, 0xC1, 0x02, 0x75, 0x02, 0x32, 0xC0, 0xC3};
        constexpr std::size_t kPlayerAudioComponentSlot = 0xB8;
        constexpr std::size_t kPlayerAudioFlags = 0x40;
        using PlayerAudioPredicateFn = bool(__fastcall*)(const void*);
        // Return sites of the twelve callers (call + 5): fire play 0x2EFA7E0
        // (x2), fire queue consumer 0x2F0DB9C, the player-or-its-vehicle test
        // 0x2F030C0 (weapon settings, fire sound choice, fire event,
        // projectile audio 0x118391C), settings variant 0x3110424, auto fire
        // 0x2F17D84, 0x2EFB1F0, 0x2EEDA7C, 0x2F09768, 0x2F0D428 (x2),
        // 0x2F19FD0.
        constexpr std::uintptr_t kPlayerAudioSites[PlayerAudioStats::kSites - 1] = {
            0x2EFA8F5, 0x2EFA923, 0x2F0DE5A, 0x2F030D2, 0x3110436, 0x2F17F2C,
            0x2EFB21F, 0x2EEDB10, 0x2F0994E, 0x2F0D4C8, 0x2F0D751, 0x2F1A0CE};
        constexpr std::size_t kPlayerAudioPeds = 4;
        std::atomic<std::uint64_t> g_playerAudioPeds[kPlayerAudioPeds]{};
        std::atomic<PlayerAudioPredicateFn> g_originalPlayerAudio{nullptr};
        void* g_playerAudioTarget = nullptr;
        std::atomic<std::uint64_t> g_playerAudioCalls{0};
        std::atomic<std::uint64_t> g_playerAudioEngineTrue{0};
        std::atomic<std::uint64_t> g_playerAudioOverrides{0};
        std::atomic<std::uint64_t> g_playerAudioSiteTrue[PlayerAudioStats::kSites]{};

        void CountPlayerAudioSite(std::uintptr_t returnAddress)
        {
            const std::uintptr_t rva = returnAddress - g_base;
            std::size_t site = PlayerAudioStats::kSites - 1;
            for (std::size_t index = 0; index + 1 < PlayerAudioStats::kSites; ++index)
            {
                if (kPlayerAudioSites[index] == rva)
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!g_base)
            g_base = reinterpret_cast<std::uintptr_t>(base);
        return Hook(base, kPlayerAudioPredicateRva, kPlayerAudioPredicateSignature,
            sizeof(kPlayerAudioPredicateSignature), reinterpret_cast<void*>(&PlayerAudioPredicateDetour),
            g_originalPlayerAudio, g_playerAudioTarget, "player_weapon_audio", error);
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
                    static_cast<unsigned>(kPlayerAudioSites[index]), static_cast<unsigned long long>(count));
            else
                std::snprintf(item, sizeof(item), "%sother:%llu", text.empty() ? "" : "|",
                    static_cast<unsigned long long>(count));
            text += item;
        }
        return text.empty() ? "none" : text;
    }

    // The player's horse variant of the ped audio (mount_audio.h).
    // The predicate, whole function (0x54 bytes, ends in ret):
    //   ent = [rcx+0B0h] ; the entity linked to the slot +0xA8 component
    //   no when ent = 0
    //   row = [0x39ECE48] + ((dword [ent+9Ch] & 1FFFFh) - [0x39ECE40]) * 148h
    //   comp = [row+0B8h] & ~1 ; flags = byte [comp+40h]
    //   yes when !(flags & 1) && (flags & 2)
    namespace
    {
        static_assert(sizeof(MountAudioStats::siteTrue) / sizeof(std::uint64_t) == replay::mountaudio::kCounterSlots,
            "one counter per site and other");
        constexpr std::uintptr_t kMountAudioPredicateRva = 0x8B69B0;
        constexpr std::uint8_t kMountAudioPredicateSignature[] = {
            0x48, 0x8B, 0x81, 0xB0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x45, 0x8B, 0x80, 0x9C, 0x00,
            0x00, 0x00, 0x25, 0xFF, 0xFF, 0x01, 0x00, 0x2B, 0x05, 0x73, 0x64, 0x13, 0x03, 0x48, 0x69, 0xC8,
            0x48, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05, 0x6D, 0x64, 0x13, 0x03, 0x48, 0x8B, 0x94, 0x01, 0xB8,
            0x00, 0x00, 0x00, 0x48, 0x8B, 0xCA, 0x48, 0x83, 0xE1, 0xFE, 0x48, 0xF7, 0xDA, 0x48, 0x1B, 0xC0,
            0x48, 0x23, 0xC1, 0x8A, 0x48, 0x40, 0xB0, 0x01, 0x84, 0xC8, 0x75, 0x05, 0xF6, 0xC1, 0x02, 0x75,
            0x02, 0x32, 0xC0, 0xC3};
        constexpr std::size_t kMountLinkSlot = 0xA8; // component row slot the predicate is given
        constexpr std::size_t kMountLinkEntity = 0xB0;
        using MountAudioPredicateFn = bool(__fastcall*)(const void*);
        constexpr std::size_t kMountAudioComponents = 4;
        std::atomic<std::uint64_t> g_mountAudioComponents[kMountAudioComponents]{};
        std::atomic<MountAudioPredicateFn> g_originalMountAudio{nullptr};
        void* g_mountAudioTarget = nullptr;
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
            const std::size_t slot = replay::mountaudio::SiteSlot(static_cast<std::uint32_t>(returnRva));
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!g_base)
            g_base = reinterpret_cast<std::uintptr_t>(base);
        return Hook(base, kMountAudioPredicateRva, kMountAudioPredicateSignature,
            sizeof(kMountAudioPredicateSignature), reinterpret_cast<void*>(&MountAudioPredicateDetour),
            g_originalMountAudio, g_mountAudioTarget, "player_mount_audio", error);
    }

    void SetPlayerMountAudioComponents(const std::vector<std::uint64_t>& components)
    {
        for (std::size_t index = 0; index < kMountAudioComponents; ++index)
            g_mountAudioComponents[index].store(index < components.size() ? components[index] : 0,
                std::memory_order_release);
    }

    // foot_material.h MovementTermCorrection: the movement volume 0x31815C4
    // (foot audio, limb data, foot, f32, out low-pass), detoured: the original
    // runs, then the -2 dB term it applies to a puppet because the hidden live
    // player is on foot is given back when the recorded state would not have
    // applied it (the player's own puppet; any puppet while the recorded
    // player rides). The predicate pattern of the mount detour: no write, no
    // call but the original and the mount predicate's original.
    namespace
    {
        constexpr std::uintptr_t kMovementVolumeRva = 0x31815C4;
        constexpr std::uint8_t kMovementVolumeSignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x83,
            0x60, 0x18, 0x00, 0x48, 0x8B, 0xD9, 0x0F, 0x29, 0x70, 0xE8, 0x0F, 0x28, 0xF3, 0x0F, 0x29, 0x78, 0xD8,
            0x0F, 0x57, 0xFF};
        constexpr std::uintptr_t kMovementTermRva = 0x3E67E5C; // f32 -2.0 (read in the exe)
        using MovementVolumeFn = float(__fastcall*)(void*, void*, std::uint8_t, float, std::uint32_t*);
        std::atomic<MovementVolumeFn> g_originalMovementVolume{nullptr};
        void* g_movementVolumeTarget = nullptr;
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
        std::atomic<bool> g_movementDetailOn{false};

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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!g_base)
            g_base = reinterpret_cast<std::uintptr_t>(base);
        float term = 0.0f;
        if (!GuardedRead(&term, base + kMovementTermRva, sizeof(term)) || term != -2.0f)
        {
            error += "movement_volume_term_mismatch;";
            return false;
        }
        g_movementTerm = term;
        return Hook(base, kMovementVolumeRva, kMovementVolumeSignature, sizeof(kMovementVolumeSignature),
            reinterpret_cast<void*>(&MovementVolumeDetour), g_originalMovementVolume, g_movementVolumeTarget,
            "movement_volume", error);
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
                    static_cast<unsigned>(replay::mountaudio::kSites[index]), static_cast<unsigned long long>(count));
            else
                std::snprintf(item, sizeof(item), "%sother:%llu", text.empty() ? "" : "|",
                    static_cast<unsigned long long>(count));
            text += item;
        }
        return text.empty() ? "none" : text;
    }

    // ---- The generic sound player's ports ----

    namespace
    {
        // The ports the generic player calls that the rest of the mod does
        // not (each signature found once in the game's two code sections; S13
        // twice with the masked displacements, made one by its tail jump). The
        // read probe's hook table has the same bytes (soundfx_probe.cpp
        // kHooks).
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
            std::uintptr_t rva;
            const char* signature;
            std::uintptr_t checkRva;  // 0: none
            const char* checkBytes;
        };
        constexpr GenericPortSpec kGenericPorts[kGenericPortCount] = {
            {"S10", 0x257CB78, "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 30 33 DB 41 8B E9 45 8A F0", 0, nullptr},
            {"S13", 0x2585BE4,
                "0F B7 91 D6 00 00 00 81 FA FF FF 00 00 74 2A 0F B6 81 A0 00 00 00 44 8B 05 ?? ?? ?? ?? 48 69 C8 60 A8 02 00 "
                "44 0F AF C2 48 8B 05 ?? ?? ?? ?? 4C 03 84 01 50 A8 02 00",
                0x2585C25, "E9 C2 0F FB FF"},
            {"S18", 0x2537168, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 F3 0F 11 8C 08 20 01 00 00 C3", 0, nullptr},
            {"S19", 0x2536E34, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 66 89 94 08 3C 01 00 00 C3", 0, nullptr},
            {"S21", 0x2536C70,
                "41 B8 64 00 00 00 41 3B D0 72 0D 41 B8 20 4E 00 00 41 3B D0 44 0F 46 C2 8B 05 ?? ?? ?? ?? 48 C1 E0 06 66 44 89 84 08 44 01 00 00 C3",
                0, nullptr},
            {"S22", 0x2536A0C, "8B 05 ?? ?? ?? ?? 41 B8 5C 5D 00 00 41 3B D0 44 0F 46 C2 48 C1 E0 06 66 44 89 84 08 46 01 00 00 C3", 0, nullptr},
            {"S23", 0x2536E5C, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 F3 0F 11 8C 08 24 01 00 00 C3", 0, nullptr},
            {"S24", 0x253717C, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 F3 0F 11 8C 08 34 01 00 00 C3", 0, nullptr},
        };
        void* g_genericPort[kGenericPortCount] = {};
        bool g_genericPortResolved = false;
        std::string g_genericPortStatus;

        using PrepareFn = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        using SoundFn1 = std::uint64_t(__fastcall*)(std::uint64_t);
        using SettingF32Fn = std::uint64_t(__fastcall*)(std::uint64_t, float);
        using SettingIntFn = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t);

        // "8B 05 ?? ?? ?? ?? 48" against the bytes at `at` (guarded read).
        bool MaskedSignatureMatch(const std::uint8_t* at, const char* text)
        {
            std::uint8_t bytes[96] = {};
            std::size_t count = 0;
            std::uint8_t mask[96] = {};
            const char* p = text;
            while (*p && count < sizeof(bytes))
            {
                while (*p == ' ')
                    ++p;
                if (!*p)
                    break;
                if (p[0] == '?' && p[1] == '?')
                {
                    mask[count++] = 0;
                    p += 2;
                    continue;
                }
                char hex[3] = {p[0], p[1], 0};
                bytes[count] = static_cast<std::uint8_t>(std::strtoul(hex, nullptr, 16));
                mask[count++] = 1;
                p += 2;
            }
            if (!count)
                return false;
            std::uint8_t found[96] = {};
            if (!replay::mem::Copy(found, at, count, replay::mem::kLowest))
                return false;
            for (std::size_t n = 0; n < count; ++n)
                if (mask[n] && found[n] != bytes[n])
                    return false;
            return true;
        }

        void ResolveGenericPorts()
        {
            if (g_genericPortResolved)
                return;
            g_genericPortResolved = true;
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            std::string status;
            for (int n = 0; n < kGenericPortCount; ++n)
            {
                const GenericPortSpec& spec = kGenericPorts[n];
                // A port the read probe (or the generic recorder) hooked:
                // its bytes start with the hook's jump, the trampoline is the function.
                void* probed = replay::soundfx::OriginalFor(spec.rva);
                const char* result = "ok";
                if (probed)
                    g_genericPort[n] = probed;
                else if (!MaskedSignatureMatch(base + spec.rva, spec.signature))
                    result = "signature_mismatch";
                else if (spec.checkRva && !MaskedSignatureMatch(base + spec.checkRva, spec.checkBytes))
                    result = "second_check_mismatch";
                else
                    g_genericPort[n] = base + spec.rva;
                status += std::string(n ? ";" : "") + spec.id + ":" + (probed ? "trampoline" : result);
            }
            g_genericPortStatus = status;
        }

        void* GenericPortFor(GenericPort port)
        {
            ResolveGenericPorts();
            return g_genericPort[port];
        }

        bool CallSettingF32(GenericPort port, std::uint64_t sound, float value)
        {
            auto set = reinterpret_cast<SettingF32Fn>(GenericPortFor(port));
            const std::uint64_t settings = set ? PortSettings(sound, kLinkAdjust) : 0;
            if (!set || !settings)
                return false;
            struct Context { SettingF32Fn set; std::uint64_t settings; float value; };
            Context context{set, settings, value};
            std::uint32_t code = 0;
            return GuardedCall([](void* raw) {
                auto* c = static_cast<Context*>(raw);
                c->set(c->settings, c->value);
            }, &context, code);
        }

        bool CallSettingInt(GenericPort port, std::uint64_t sound, std::uint64_t value)
        {
            auto set = reinterpret_cast<SettingIntFn>(GenericPortFor(port));
            const std::uint64_t settings = set ? PortSettings(sound, kLinkAdjust) : 0;
            if (!set || !settings)
                return false;
            struct Context { SettingIntFn set; std::uint64_t settings; std::uint64_t value; };
            Context context{set, settings, value};
            std::uint32_t code = 0;
            return GuardedCall([](void* raw) {
                auto* c = static_cast<Context*>(raw);
                c->set(c->settings, c->value);
            }, &context, code);
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
            // constructor 0x487E34 zeroes and S39 0x2573638 appends to). The
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
        // makes for a request's owner (0x312D9C5, create flag 0): 0xE383E8
        // asks the entity's audio (its vtable +0xF0) for the group (that
        // object's vtable +0x78). Signature checked once, SEH.
        constexpr std::uintptr_t kEntityEnvGroupRva = 0xE383E8;
        constexpr std::uint8_t kEntityEnvGroupSignature[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x01, 0x8A,
            0xDA, 0xFF, 0x90, 0xF0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xC8, 0x33, 0xC0, 0x48, 0x85, 0xC9, 0x74, 0x09, 0x4C, 0x8B,
            0x01, 0x8A, 0xD3, 0x41, 0xFF, 0x50, 0x78, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
        using EntityEnvGroupFn = std::uint64_t(__fastcall*)(std::uint64_t, std::uint8_t);

        std::uint64_t EntityEnvironmentGroup(std::uint64_t entity, const char*& status)
        {
            static int checked = 0; // 0 not yet, 1 matched, -1 mismatch
            static EntityEnvGroupFn fn = nullptr;
            if (!checked)
            {
                auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
                checked = GuardedCompare(base + kEntityEnvGroupRva, kEntityEnvGroupSignature, sizeof(kEntityEnvGroupSignature))
                    ? 1 : -1;
                if (checked > 0)
                    fn = reinterpret_cast<EntityEnvGroupFn>(base + kEntityEnvGroupRva);
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
            struct Context { EntityEnvGroupFn fn; std::uint64_t entity; std::uint64_t group; };
            Context context{fn, entity, 0};
            std::uint32_t code = 0;
            if (!GuardedCall([](void* raw) {
                    auto* c = static_cast<Context*>(raw);
                    c->group = c->fn(c->entity, 0);
                }, &context, code))
            {
                status = "env_exception";
                return 0;
            }
            if (context.group && (context.group & 7u) != 0)
            {
                status = "env_misaligned";
                return 0;
            }
            status = context.group ? "env_given" : "env_none";
            return context.group;
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        static PrepareAndPlayFn playFn = nullptr;
        static SoundParamsReleaseFn release = nullptr;
        if (!playFn)
        {
            void* probed = replay::soundfx::OriginalFor(kPrepareAndPlayRva);
            if ((!probed &&
                    !GuardedCompare(base + kPrepareAndPlayRva, kPrepareAndPlaySignature, sizeof(kPrepareAndPlaySignature))) ||
                !GuardedCompare(base + kSoundParamsReleaseRva, kSoundParamsReleaseSignature, sizeof(kSoundParamsReleaseSignature)))
            {
                error = "persistent_signature_mismatch";
                return false;
            }
            playFn = reinterpret_cast<PrepareAndPlayFn>(probed ? probed : base + kPrepareAndPlayRva);
            release = reinterpret_cast<SoundParamsReleaseFn>(base + kSoundParamsReleaseRva);
        }
        // Through the trampolines: the replayed create is not recorded again.
        const PersistentByRefFn byRef = g_originalByRef.load(std::memory_order_acquire);
        const PersistentByHashFn byHashFn = g_originalByHash.load(std::memory_order_acquire);
        const SoundParamsCtorFn construct = g_originalCtor.load(std::memory_order_acquire);
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
            PersistentByRefFn byRef; PersistentByHashFn byHash; bool useHash; SoundParamsCtorFn construct;
            SoundParamsReleaseFn release; PrepareAndPlayFn play; bool doPlay; std::uint64_t audio; std::uint64_t soundRef;
            const GenericSoundInit* init; std::uint64_t* slot;
        };
        Context context{byRef, byHashFn, byHash, construct, release, playFn, play, audio, soundRef, &init, slot};
        std::uint32_t code = 0;
        const bool ok = GuardedCall([](void* raw) {
            auto* c = static_cast<Context*>(raw);
            alignas(16) std::uint8_t params[kSoundParamsBytes + 0x40] = {};
            c->construct(params);
            if (c->init->recordedParams)
            {
                // The recorded params whole (the category the game gave, every
                // value field), as mode c's queue; the flags are the player's
                // (mode a detaches), the position the player's when it gives
                // one.
                CopyRecordedSoundParams(params, c->init->recordedParams);
                params[0x13A] = c->init->flags;
                if (c->init->hasPosition)
                    std::memcpy(params + 0x00, c->init->position, sizeof(c->init->position));
                const std::uint64_t none = 0;
                std::memcpy(params + 0xC8, &none, 8); // no tracker
            }
            else
            {
                WriteGenericValues(params, *c->init); // the game's value fields
                WriteGenericInit(params, *c->init); // tracker [0xC8, 0xE8) left clear: the player drives the position
                if (c->init->listParams)
                    std::memcpy(params + 0x10C, c->init->listParams + 0x10C, 0x128 - 0x10C);
                if (c->init->category && KnownSoundCategory(c->init->category))
                    std::memcpy(params + kSoundCategoryOffset, &c->init->category, sizeof(c->init->category));
            }
            // The group goes in +0xE0; the creator hands it to S30, which
            // counts the use (the settings' release gives it back).
            std::memcpy(params + 0xE0, &c->init->envGroupGiven, 8);
            if (c->useHash)
                c->byHash(c->audio, static_cast<std::uint32_t>(c->soundRef), reinterpret_cast<std::uint64_t>(c->slot),
                    reinterpret_cast<std::uint64_t>(params));
            else
                c->byRef(c->audio, c->soundRef, reinterpret_cast<std::uint64_t>(c->slot), reinterpret_cast<std::uint64_t>(params));
            c->release(params + 0x12C);
            if (*c->slot && c->doPlay)
                c->play(*c->slot, 0, 0, 0, 0);
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
            // Why, as PlayDirectSound tells it (0x2588950 / 0x2589A40 and the
            // persistent cores 0x258922E / 0x2589101): the audio entity's state
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

    namespace
    {
        std::atomic<bool> g_queueEnvGroup{true}; // SoundQueueEnvGroup
    }
    void SetQueueEnvGroup(bool on) { g_queueEnvGroup.store(on, std::memory_order_relaxed); }

    bool QueueGenericSound(std::uint64_t audio, std::uint64_t soundRef, const std::uint8_t* recorded,
        GenericSoundInit& init, std::string& error, std::uint64_t* ticket)
    {
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        static CreateAndPlayFn play = nullptr;
        static SoundParamsReleaseFn release = nullptr;
        if (!play)
        {
            if (!GuardedCompare(base + kCreateAndPlayPublicRva, kCreateAndPlayPublicSignature,
                    sizeof(kCreateAndPlayPublicSignature)) ||
                !GuardedCompare(base + kSoundParamsReleaseRva, kSoundParamsReleaseSignature, sizeof(kSoundParamsReleaseSignature)))
            {
                error = "queue_signature_mismatch";
                return false;
            }
            play = reinterpret_cast<CreateAndPlayFn>(base + kCreateAndPlayPublicRva);
            release = reinterpret_cast<SoundParamsReleaseFn>(base + kSoundParamsReleaseRva);
        }
        const SoundParamsCtorFn construct = g_originalCtor.load(std::memory_order_acquire);
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
        if (init.envGroupEntity && g_queueEnvGroup.load(std::memory_order_relaxed))
            init.envGroupGiven = EntityEnvironmentGroup(init.envGroupEntity, init.envGroupStatus);
        ExpectQueuedCreate(audio, soundRef);
        const std::uint64_t opened = OpenQueueTicket(audio, soundRef);
        if (ticket)
            *ticket = opened;
        struct Context
        {
            CreateAndPlayFn play; SoundParamsCtorFn construct; SoundParamsReleaseFn release; std::uint64_t audio;
            std::uint64_t soundRef; const std::uint8_t* recorded; const GenericSoundInit* init; bool queued;
        };
        Context context{play, construct, release, audio, soundRef, recorded, &init, false};
        std::uint32_t code = 0;
        bool ok = false;
        {
            ReplayEmitScope emit; // the S03 refusal of a silenced puppet's own one-shot lets the replay's through
            ok = GuardedCall([](void* raw) {
                auto* c = static_cast<Context*>(raw);
                alignas(16) std::uint8_t params[kSoundParamsBytes + 0x40] = {};
                c->construct(params);
                if (c->recorded)
                    CopyRecordedSoundParams(params, c->recorded);
                else
                {
                    WriteGenericValues(params, *c->init);
                    WriteGenericInit(params, *c->init);
                }
                const std::uint64_t none = 0;
                std::memcpy(params + 0xC8, &none, 8); // no tracker
                if (c->init->envGroupGiven) // the game's weapon rattle does the same (0x31907EA)
                    std::memcpy(params + 0xE0, &c->init->envGroupGiven, 8);
                c->queued = c->play(c->audio, c->soundRef, reinterpret_cast<std::uint64_t>(params));
                c->release(params + 0x12C);
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
        auto prepare = reinterpret_cast<PrepareFn>(GenericPortFor(kGenericS10));
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
        struct Context { PrepareFn prepare; std::uint64_t sound; std::uint64_t result; };
        Context context{prepare, sound, 0};
        std::uint32_t code = 0;
        if (!GuardedCall([](void* raw) {
                auto* c = static_cast<Context*>(raw);
                c->result = c->prepare(c->sound, 0, 1, 0);
            }, &context, code))
        {
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
        constexpr std::uintptr_t kRequiredDynamicBankRva = 0x2EF341C;
        constexpr std::uint8_t kRequiredDynamicBankSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55,
            0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x40, 0x49, 0x8B, 0xF0};
        constexpr std::uintptr_t kFindBankSlotRva = 0x2545B1C;
        constexpr std::uint8_t kFindBankSlotSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xD9, 0x48, 0x8D, 0x0D};
        constexpr std::uintptr_t kBankLoadStatusRva = 0x2549530;
        constexpr std::uint8_t kBankLoadStatusSignature[] = {
            0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x8B, 0xDA, 0x48, 0x8D, 0x0D};
        using RequiredDynamicBankFn = std::uint32_t*(__fastcall*)(std::uint32_t*, std::uint64_t, std::uint32_t*);
        using FindBankSlotFn = std::uint64_t(__fastcall*)(std::uint32_t);
        using BankLoadStatusFn = std::uint32_t(__fastcall*)(std::uint64_t, std::uint32_t);
        // The banks a ref needs do not change: one metadata walk per ref.
        std::unordered_map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> g_bankOfRef;
    }

    bool ProbeSoundBank(std::uint64_t soundRef, BankProbe& out, std::string& error)
    {
        out = BankProbe{};
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        static int checked = 0; // 0 not yet, 1 ok, -1 refused
        static RequiredDynamicBankFn required = nullptr;
        static FindBankSlotFn findSlot = nullptr;
        static BankLoadStatusFn loadStatus = nullptr;
        if (checked == 0)
        {
            const bool ok = GuardedCompare(base + kRequiredDynamicBankRva, kRequiredDynamicBankSignature,
                                sizeof(kRequiredDynamicBankSignature)) &&
                GuardedCompare(base + kFindBankSlotRva, kFindBankSlotSignature, sizeof(kFindBankSlotSignature)) &&
                GuardedCompare(base + kBankLoadStatusRva, kBankLoadStatusSignature, sizeof(kBankLoadStatusSignature));
            checked = ok ? 1 : -1;
            if (ok)
            {
                required = reinterpret_cast<RequiredDynamicBankFn>(base + kRequiredDynamicBankRva);
                findSlot = reinterpret_cast<FindBankSlotFn>(base + kFindBankSlotRva);
                loadStatus = reinterpret_cast<BankLoadStatusFn>(base + kBankLoadStatusRva);
            }
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
            RequiredDynamicBankFn required; FindBankSlotFn findSlot; BankLoadStatusFn loadStatus;
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
                if (!c->known)
                    c->required(&c->bank, c->ref, &c->count);
                if (c->count == 0)
                    return;
                c->slot = c->findSlot(c->bank);
                if (c->slot)
                    c->status = c->loadStatus(c->slot, c->bank);
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
        auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        static PrepareAndPlayFn play = nullptr;
        if (!play)
        {
            void* probed = replay::soundfx::OriginalFor(kPrepareAndPlayRva);
            if (!probed && !GuardedCompare(base + kPrepareAndPlayRva, kPrepareAndPlaySignature, sizeof(kPrepareAndPlaySignature)))
            {
                error = "persistent_signature_mismatch";
                return false;
            }
            play = reinterpret_cast<PrepareAndPlayFn>(probed ? probed : base + kPrepareAndPlayRva);
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
        struct Context { PrepareAndPlayFn play; std::uint64_t sound; };
        Context context{play, sound};
        std::uint32_t code = 0;
        if (!GuardedCall([](void* raw) {
                auto* c = static_cast<Context*>(raw);
                c->play(c->sound, 0, 0, 0, 0);
            }, &context, code))
        {
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
        auto stop = reinterpret_cast<SoundFn1>(GenericPortFor(kGenericS13));
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
        struct Context { SoundFn1 stop; std::uint64_t sound; };
        Context context{stop, sound};
        std::uint32_t code = 0;
        if (!GuardedCall([](void* raw) {
                auto* c = static_cast<Context*>(raw);
                c->stop(c->sound);
            }, &context, code))
        {
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

    bool SetSoundVolume(std::uint64_t sound, float volume) { return CallSettingF32(kGenericS18, sound, volume); }
    bool SetSoundPitch(std::uint64_t sound, std::int16_t pitch)
    {
        return CallSettingInt(kGenericS19, sound, static_cast<std::uint64_t>(static_cast<std::uint16_t>(pitch)));
    }
    bool SetSoundLowPass(std::uint64_t sound, std::uint32_t cutoff) { return CallSettingInt(kGenericS21, sound, cutoff); }
    bool SetSoundHighPass(std::uint64_t sound, std::uint32_t cutoff) { return CallSettingInt(kGenericS22, sound, cutoff); }
    bool SetSoundPostSubmix(std::uint64_t sound, float attenuation) { return CallSettingF32(kGenericS23, sound, attenuation); }
    bool SetSoundVolumeCurveScale(std::uint64_t sound, float scale) { return CallSettingF32(kGenericS24, sound, scale); }

    std::string GenericSoundPortStatus()
    {
        ResolveGenericPorts();
        return g_genericPortStatus;
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
