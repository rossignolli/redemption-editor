#pragma once

// Closed theater with the engine's own functions. While the replay owns the
// world (WorldModel=gta, from the clear on the first playback until the save
// reload), the replay takes the world over like a replay editor does:
//   - persistent characters despawned and held (the PERSCHAR native's
//     handler, flag [[manager]+0x10]);
//   - every vehicle removed with the vehicle reuse pool (RemoveAllVehs(false),
//     which ends with FlushVehicleReusePool);
//   - every ped but the player removed (RemoveAllPedsWithException(player,
//     4 = ForceAllRandomAndMission, nullptr, true, true));
//   - the game's script threads paused every tick (_PAUSE_SCRIPT_THREADS),
//     so a thread launched after the clear sleeps too;
//   - ambient population denied at its single funnel CreatePopulationPed;
//   - the static emitters reset (StopAndResetStaticEmitters on the static
//     emitter manager).
// Evidence (log only): every ped left after the clear and why, the spawn
// funnel's callers during the replay, the mix scenes started (MixerStartScene)
// and the speech state 3 refusals (SpeechState3Rules).
//
// Each internal function is found by its signature (game_sig_core.h) and by
// the call or field sites the use relies on; one not found turns that feature
// off for the session and logs it (by the names below in quotes).

#include "engine_sigs.h"
#include "game_sig_core.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace replay::theatre
{
    // ---- ini ([RedemptionEditor]) ------------------------------------------

    // Each step has its own key so the bench measures them one at a time.
    struct Config
    {
        bool perschar = true;     // TheatrePerschar
        bool vehicles = true;     // TheatreVehicles: RemoveAllVehs(false)
        bool peds = true;         // TheatrePeds: RemoveAllPedsWithException mode 4
        bool pauseScripts = true; // TheatrePauseScripts
        // The threads ScriptHookRDR2 runs the other ASIs in (a camera mod, a
        // trainer) are left running (UnpauseForScriptHook).
        // While the quit alert or the pause menu is up (script.cpp
        // FrontendScreenUp): FrontendPrompts=1 (developer key, default 1)
        // lets the game's button prompts through (the alert's "Yes / No"
        // are prompts: _UI_PROMPT_DISABLE_PROMPTS_THIS_FRAME hid them and
        // took their input); TheatreFrontendScripts=1 (developer key,
        // default 0) also lets the game's script threads run. Bisect keys.
        bool frontendPrompts = true;
        bool frontendScripts = false;
        bool denySpawn = true;    // TheatreDenySpawn
        bool emitters = true;     // TheatreEmitters
        // TheatreHooks: which evidence detours are installed (bit 1 spawn
        // funnel, 2 mix scene start, 4 speech state 3 rules, 8 the state 3
        // handler's failure code (kHookState3), 16 the mix scene stop
        // (kHookSceneStop)).
        std::uint32_t hooks = 31;
    };
    constexpr std::uint32_t kHookSpawn = 1;
    constexpr std::uint32_t kHookScene = 2;
    constexpr std::uint32_t kHookRules = 4;

    // ---- engine functions ---------------------------------------------------

    // "CreatePopulationPed", the spawn funnel (hooked).
    inline const sig::Signature kSpawnFunnel{"theatre_create_population_ped",
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 4C 89 70 20 55 48 8D 68 F1",
        0xBF23D4};
    // "RemoveAllVehs" calls "FlushVehicleReusePool" before its own deferred
    // pass ("vehicles_reuse_pool").
    inline const sig::Signature kRemoveAllVehs{"theatre_remove_all_vehs",
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 8A 05 ?? ?? ?? ?? 44 8A F1 84 C0 "
        "75 04 33 DB EB 36",
        0x134E95C};
    inline const sig::Signature kVehicleReuseCall{"theatre_vehicles_reuse_pool_call", kRemoveAllVehs, sig::Offset{0xE1},
        "E8 ?? ?? ?? ??", 0x134EA3D};
    inline const sig::Signature kFlushVehicleReuse{"theatre_flush_vehicle_reuse_pool", kVehicleReuseCall,
        sig::Rel32{0x01, 0x05},
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 8B 15 ?? ?? ?? ??", 0x130DD84};
    // "RemoveAllPedsWithException" ends with FlushPedQueues(1, 1)
    // ("peds_flush_queues") and a jmp to the deferred ped destruction
    // ("peds_deferred_destroy", the ped destruction of -1).
    inline const sig::Signature kRemoveAllPeds{"theatre_remove_all_peds",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 8A 05 ?? ?? ?? ?? 41 8A E9 "
        "4D 8B F8 44 8B E2 4C 8B F1",
        0xC0F83C};
    // Its reads of the stack arguments (engine_calls.h): a build that
    // changed them fails here.
    inline const sig::Signature kRemoveAllPedsArg5{"theatre_remove_all_peds_arg5", kRemoveAllPeds, sig::Offset{0x272},
        "8A 54 24 70", 0xC0FAAE};
    inline const sig::Signature kPedFlushCall{"theatre_peds_flush_queues_call", kRemoveAllPeds, sig::Offset{0x32B},
        "E8 ?? ?? ?? ??", 0xC0FB67};
    inline const sig::Signature kPedFlushQueues{"theatre_flush_ped_queues", kPedFlushCall, sig::Rel32{0x01, 0x05},
        "40 53 48 83 EC 20 8A DA 84 C9 74 52 EB 07 33 C9 E8 ?? ?? ?? ??", 0xC03C84};
    inline const sig::Signature kPedDestroyJump{"theatre_peds_deferred_destroy_jump", kRemoveAllPeds,
        sig::Offset{0x34C}, "E9 ?? ?? ?? ??", 0xC0FB88};
    inline const sig::Signature kPedDeferredDestroy{"theatre_peds_deferred_destroy", kPedDestroyJump,
        sig::Rel32{0x01, 0x05}, "48 83 EC 28 83 C9 FF E8 ?? ?? ?? ??", 0x73381C};
    // The audio reset passes the static emitter manager by lea rcx to
    // "StopAndResetStaticEmitters" ("emitter_manager").
    inline const sig::Signature kAudioReset{"theatre_audio_reset",
        "48 83 EC 28 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 13", 0x2F191D0};
    inline const sig::Signature kEmitterReset{"theatre_emitter_reset", kAudioReset, sig::Offset{0xBA},
        "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ??", 0x2F1928A};
    inline const sig::Signature kStaticEmitterManager{"theatre_static_emitter_manager", kEmitterReset,
        sig::Rel32{0x03, 0x07}, "", 0x5C505C0};
    inline const sig::Signature kStaticEmitters{"theatre_stop_and_reset_static_emitters", kEmitterReset,
        sig::Rel32{0x08, 0x0C},
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 33 F6 48 8B F9 39 B1 C0 D0 01 00", 0x312F7A4};
    // "SpeechState3Rules" (hooked).
    inline const sig::Signature kSpeechRules{"theatre_speech_state3_rules",
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 57 48 83 EC 20 F6 81 E0 03 00 00 01",
        0x2F972C4};

    // The speech manager's per-owner release of the ambient slots (see
    // speech_slots.h; "FreeAmbientSlotsOfOwner"(manager, speechEntity): every
    // ambient slot whose +0x28 is the entity goes through the slot release,
    // which frees it only when each user of its reference says it may go).
    // Its one caller is the speech entity's destructor, which passes the
    // speech manager by lea rcx ("speech_manager").
    inline const sig::Signature kSpeechEntityDestructor{"theatre_speech_entity_destructor",
        "40 53 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 48 8B 41 08 B9 FF FF 00 00", 0x2F9316C};
    inline const sig::Signature kAmbientReleaseCall{"theatre_ambient_release_call", kSpeechEntityDestructor,
        sig::Offset{0x25}, "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ??", 0x2F93191};
    inline const sig::Signature kSpeechManager{"theatre_speech_manager", kAmbientReleaseCall, sig::Rel32{0x03, 0x07},
        "", 0x5BB9750};
    inline const sig::Signature kAmbientRelease{"theatre_free_ambient_slots_of_owner", kAmbientReleaseCall,
        sig::Rel32{0x08, 0x0C},
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 33 DB 48 8B F2 48 8B F9 66 3B 99 F8 03 00 00", 0x2FB1E94};
    // The speech manager's ambient slot array and count, as
    // FreeAmbientSlotsOfOwner reads them (+0x3F0, u16 at +0x3F8) and the
    // stride (0x38).
    constexpr std::uint32_t kAmbientArray = 0x3F0;
    constexpr std::uint32_t kAmbientCount = 0x3F8;
    constexpr std::uint32_t kAmbientStride = 0x38;
    // "SpeechState3Handler", the state 3 handler of a line (the line
    // dispatcher calls it and stops the line with reason 6 when the code is a
    // failure: below 0x46). Its own failures: 0xB (no position), 0xA (the
    // data's play refused), or the rule code of speech entity vtable +0x68.
    // A log-only detour records the code (TheatreHooks bit 8).
    inline const sig::Signature kSpeechState3{"theatre_speech_state3_handler",
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 24 A9 48 81 EC 90 00 00 00 48 8B D9 48 8B 89 C0 01 00 00",
        0x2FC486C};
    constexpr std::uint32_t kHookState3 = 8;
    // "MixerStopScene"(scene, byte) (see mix_scene_core.h), 97 callers,
    // STOP_AUDIO_SCENE's among them; the scene's hash is at +0x328 (written by
    // the start). A log/record-only detour (TheatreHooks bit 16): with the
    // start detour it gives each scene's start and stop while SetSceneObserve
    // is on. The two functions: engine_sigs.h kMixerStartScene, kMixerStopScene.
    constexpr std::uint32_t kHookSceneStop = 16;
    constexpr std::uint32_t kMixSceneHashOffset = 0x328;
    // The conversation scene's start passes the conversation's +0xE8 as the
    // scene's out pointer; the participants are at +0x30 (array, stride 0xA0)
    // and +0x38 (u16 count), each with its tag at +0x7C and the "has lines"
    // byte at +0x98. The start detour knows the conversation's call
    // (engine_sigs.h kConversationSceneCall) by its return address: the call + 5.
    constexpr std::uint32_t kConversationCallLength = 5;
    constexpr std::uint32_t kConversationSceneSlot = 0xE8;
    constexpr std::uint32_t kConversationParticipants = 0x30;
    constexpr std::uint32_t kConversationParticipantCount = 0x38;
    constexpr std::uint32_t kParticipantStride = 0xA0;
    constexpr std::uint32_t kParticipantTag = 0x7C;
    constexpr std::uint32_t kParticipantFlag = 0x98;
    constexpr std::uint32_t kParticipantReadMax = 32;
    // Codes of the line result test: 0x46 and above are success (0x49 to 0x4D seen).
    constexpr bool SpeechCodeFailed(std::uint32_t code) { return code < 0x46; }

    // The PERSCHAR native, found by its hash where the natives are registered
    // (lea rdx, handler; mov rcx, hash): its handler despawns, then sets
    // [[manager]+0x10] = 1 ("perschar_manager").
    inline const sig::Signature kPerscharNative{"theatre_perschar_native",
        "48 8D 15 ?? ?? ?? ?? 48 B9 E0 3C 49 22 08 0B 69 92 E8 ?? ?? ?? ??", 0x1007C74};
    inline const sig::Signature kPerscharHandler{"theatre_perschar_handler", kPerscharNative, sig::Rel32{0x03, 0x07},
        "48 83 EC 28 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? C6 40 10 01 48 83 C4 28 C3", 0xFB3914};
    inline const sig::Signature kPerscharManager{"theatre_perschar_manager", kPerscharHandler, sig::Rel32{0x0C, 0x10},
        "", 0x4A35800};
    constexpr std::uint32_t kPerscharHeldOffset = 0x10;

    // ---- the animal bank picker (animal_bank.h) ----------------------------
    // The type picker "AnimalBankPick"(manager) -> type (0: none), called
    // once, by the manager's animal bank update ("animal_bank_pick_call", only
    // with no load pending). The detour returns the held type or 0
    // (animal_bank.h DecidePick); it reads the records and writes only the
    // held type's next bank number, inside the manager's own update, before
    // the update reads it ("AnimalBankNext").
    inline const sig::Signature kAnimalBankUpdate{"theatre_animal_bank_update",
        "48 8B C4 48 89 58 18 48 89 68 20 56 57 41 54 41 56 41 57 48 81 EC D0 00 00 00 0F B7 91 48 08 00 00",
        0x2FC65E0};
    inline const sig::Signature kAnimalBankPickCall{"theatre_animal_bank_pick_call", kAnimalBankUpdate,
        sig::Offset{0x142}, "E8 ?? ?? ?? ??", 0x2FC6722};
    inline const sig::Signature kAnimalBankPick{"theatre_animal_bank_pick", kAnimalBankPickCall, sig::Rel32{0x01, 0x05},
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 57 48 83 EC 30 33 DB 0F 29 70 E8",
        0x2F985B0};
    // The loader's use of the record: the bank count (+0x8B8,
    // "AnimalBankCount") before the slot finder, the next bank number
    // (+0x8B4, "AnimalBankNext") given to the name builder.
    inline const sig::Signature kAnimalBankCount{"theatre_animal_bank_count", kAnimalBankUpdate, sig::Offset{0x155},
        "4C 8D 3C F6 46 39 A4 FF B8 08 00 00 0F 86 ?? ?? ?? ?? 8B D6 48 8B CF E8 ?? ?? ?? ??", 0x2FC6735};
    inline const sig::Signature kAnimalBankNext{"theatre_animal_bank_next", kAnimalBankUpdate, sig::Offset{0x1C0},
        "48 8D 1C F6 44 8B 84 DF B4 08 00 00 4C 8D 4C 24 40 48 89 44 24 28 8B D6", 0x2FC67A0};
    // The animal voice of a type: the holder object (+0x8A8, "AnimalVoiceHolder"),
    // the holder (+0x898), then the entry's state 5 and its voice +0x30
    // ("AnimalVoiceEntry").
    inline const sig::Signature kAnimalVoice{"theatre_animal_voice",
        "48 89 5C 24 08 57 48 83 EC 20 33 DB 83 FA 6C", 0x2FA6674};
    inline const sig::Signature kAnimalVoiceHolder{"theatre_animal_voice_holder", kAnimalVoice, sig::Offset{0x17},
        "48 39 9C D1 A8 08 00 00 74 2E 48 8D 0C D1 48 81 C1 98 08 00 00 E8 ?? ?? ?? ??", 0x2FA668B};
    inline const sig::Signature kAnimalVoiceEntry{"theatre_animal_voice_entry", kAnimalVoice, sig::Offset{0x39},
        "83 78 20 05 75 10 48 8B 00 48 8B CF FF 50 20 84 C0 74 03 8B 5F 30 8B C3", 0x2FA66AD};
    // The release pass: the kept list +0x26D8 bounded by u16 [+0x848]
    // ("AnimalBankKeep").
    inline const sig::Signature kAnimalBankKeep{"theatre_animal_bank_keep", kAnimalBankUpdate, sig::Offset{0x58},
        "44 0F B7 CA 41 8B CC 45 85 C9 74 17 48 8D 87 D8 26 00 00 44 39 00 74 1C", 0x2FC6638};
    // The name builder's type name table (.data, char* per type,
    // "animal_name_table") and its format ("animal_bank_format", "%s_NEAR_%.2u").
    inline const sig::Signature kAnimalNameBuilder{"theatre_animal_name_builder",
        "4C 8B DC 49 89 5B 08 49 89 73 18 49 89 7B 20 55 41 55", 0x2F9BD4C};
    inline const sig::Signature kAnimalNameTableRead{"theatre_animal_name_table_read", kAnimalNameBuilder,
        sig::Offset{0x38}, "48 8D 05 ?? ?? ?? ??", 0x2F9BD84};
    inline const sig::Signature kAnimalNameTable{"theatre_animal_name_table", kAnimalNameTableRead,
        sig::Rel32{0x03, 0x07}, "", 0x3E276A0};
    inline const sig::Signature kAnimalBankFormatRead{"theatre_animal_bank_format_read", kAnimalNameBuilder,
        sig::Offset{0x4B}, "4C 8D 05 ?? ?? ?? ??", 0x2F9BD97};
    inline const sig::Signature kAnimalBankFormat{"theatre_animal_bank_format", kAnimalBankFormatRead,
        sig::Rel32{0x03, 0x07}, "", 0x37FEC88};

    // RemoveAllPedsWithException's modes: 4 deletes every
    // ped but the player, the exception and its mount, clones and a class of
    // population type 1; the script extension is taken off first.
    constexpr int kRemovePedsForceAllRandomAndMission = 4;

    // Speech manager layout (offline, from the slot finders): two bank slot
    // classes of 0x48 bytes (+0x3C0/+0x3C8, +0x3D0/+0x3D8), one of 0x68
    // (+0x840/+0x848), the ambient slots of 0x38 (+0x3F0/+0x3F8); pointer then
    // a u16 count. In each slot +0x10 is the wave slot; a bank slot holds its
    // bank at +0x2C; an ambient slot its owner (speech entity) at +0x28.
    struct SlotClass
    {
        const char* name;
        std::uint32_t array;
        std::uint32_t count;
        std::uint32_t stride;
    };
    constexpr SlotClass kSlotClasses[] = {
        {"bank_a", 0x3C0, 0x3C8, 0x48},
        {"bank_b", 0x3D0, 0x3D8, 0x48},
        {"bank_c", 0x840, 0x848, 0x68},
        {"ambient", 0x3F0, 0x3F8, 0x38},
    };
    constexpr std::uint32_t kSlotWave = 0x10;
    constexpr std::uint32_t kBankSlotBank = 0x2C;
    constexpr std::uint32_t kAmbientSlotOwner = 0x28;
    constexpr std::uint32_t kSpeechEntityPed = 0x98; // speechEntity+0x98 is the ped

    // ---- census of the peds left after the clear (evidence) ----------------

    struct PedFacts
    {
        bool player = false;
        bool dead = false;
        bool animal = false;
        bool perschar = false;    // _GET_PERSCHAR_INDEX_FROM_PED_INDEX != 0
        bool mission = false;      // IS_ENTITY_A_MISSION_ENTITY
        bool scripted = false;     // _GET_ENTITY_SCRIPT != 0
        bool inVehicle = false;
        bool onMount = false;
        bool playerGroup = false;
        bool pendingDestroy = false; // bit 21 of [ped+0x3F20]
        bool scene = false;          // bit 28 of [ped+0x3F30]
        int population = -1;         // GET_ENTITY_POPULATION_TYPE
    };

    enum class LeftReason : std::uint8_t
    {
        Player,
        PendingDestroy,
        Perschar,
        Mission,
        Scripted,
        PlayerGroup,
        Rider,
        Scene,
        Dead,
        Other,
        Count
    };

    inline const char* LeftReasonName(LeftReason reason)
    {
        switch (reason)
        {
        case LeftReason::Player: return "player";
        case LeftReason::PendingDestroy: return "pending_destroy";
        case LeftReason::Perschar: return "perschar";
        case LeftReason::Mission: return "mission";
        case LeftReason::Scripted: return "scripted";
        case LeftReason::PlayerGroup: return "player_group";
        case LeftReason::Rider: return "rider";
        case LeftReason::Scene: return "scene";
        case LeftReason::Dead: return "dead";
        default: return "other";
        }
    }

    // The first veto that explains the ped, in the order the engine's
    // deletion check and the CLEAR_AREA callback test them.
    inline LeftReason ClassifyLeft(const PedFacts& facts)
    {
        if (facts.player)
            return LeftReason::Player;
        if (facts.pendingDestroy)
            return LeftReason::PendingDestroy;
        if (facts.perschar)
            return LeftReason::Perschar;
        if (facts.mission)
            return LeftReason::Mission;
        if (facts.scripted)
            return LeftReason::Scripted;
        if (facts.playerGroup)
            return LeftReason::PlayerGroup;
        if (facts.inVehicle || facts.onMount)
            return LeftReason::Rider;
        if (facts.scene)
            return LeftReason::Scene;
        if (facts.dead)
            return LeftReason::Dead;
        return LeftReason::Other;
    }

    // ---- the spawn funnel's callers (labels the game itself passes) --------

    // Copies the funnel's source name into `out` (always terminated).
    inline void CopyLabel(const char* text, std::size_t textSize, char* out, std::size_t outSize)
    {
        if (!out || !outSize)
            return;
        std::size_t n = 0;
        for (; text && n + 1 < outSize && n < textSize && text[n]; ++n)
        {
            const char c = text[n];
            out[n] = (c == ',' || c == '\n' || c == '\r' || static_cast<unsigned char>(c) < 0x20) ? '_' : c;
        }
        out[n] = '\0';
    }

    // ---- the other ASIs' script threads ------------------------------------
    //
    // ScriptHookRDR2 runs every ASI in a game script thread of its own
    // (script hash joaat("audiotest")) and hooks GtaThread::Run.
    // _PAUSE_SCRIPT_THREADS(TRUE) ("PauseScriptThreads") sets the pause byte
    // +0x71A of every thread with an id (+0x8) but the calling one (the
    // current thread) and those whose +0x71B is 0; the thread tick
    // ("GtaThreadTick") returns at once while +0x71A is set. The replay's own
    // thread is the caller and keeps running; every other ASI's thread sleeps
    // until the reload.
    //
    // While the replay pauses the game's scripts, the threads ScriptHook
    // started (script hash joaat("audiotest"), written by the thread reset
    // into +0xC) get their pause byte back to 0 right after the native, in
    // the same tick (the unpause writes the same byte): the native's own
    // effect undone for those threads only.
    //
    // The pause loop: movzx r9d, word [the thread count]; mov rax, [the
    // thread array] ("script_threads": the count is the word just after the
    // array's qword).
    inline const sig::Signature kPauseThreads{"theatre_pause_script_threads",
        "44 0F B7 0D ?? ?? ?? ?? 45 33 DB 4C 8B D1 45 8B C3 66 45 3B D9 73 3D 48 8B 05 ?? ?? ?? ??", 0xEED6B8};
    inline const sig::Signature kThreadCount{"theatre_script_thread_count", kPauseThreads, sig::Rel32{0x04, 0x08}, "",
        0x59B29D0};
    inline const sig::Signature kThreadArray{"theatre_script_threads", kPauseThreads, sig::Rel32{0x1A, 0x1E}, "",
        0x59B29C8};
    // The pause loop's body ("PauseScriptThreadsMark"): cmp byte [rcx+0x71B],
    // r11b; je; mov byte [rcx+0x71A], 1; and the count read again.
    inline const sig::Signature kPauseThreadsMark{"theatre_pause_script_threads_mark", kPauseThreads, sig::Offset{0x30},
        "44 38 99 1B 07 00 00 74 0F C6 81 1A 07 00 00 01 44 0F B7 0D ?? ?? ?? ??", 0xEED6E8};
    inline const sig::Signature kThreadCountMark{"theatre_script_thread_count_mark", kPauseThreadsMark,
        sig::Rel32{0x14, 0x18}, "", 0x59B29D0};
    // The thread tick: cmp byte [rcx+0x71A], 0 at its top.
    inline const sig::Signature kThreadTick{"theatre_gta_thread_tick",
        "48 89 5C 24 08 57 48 83 EC 20 80 B9 1A 07 00 00 00 8B FA 48 8B D9 74 05", 0xEF73AC};
    constexpr std::uint32_t kThreadIdOffset = 0x8;
    constexpr std::uint32_t kThreadScriptHashOffset = 0xC;
    constexpr std::uint32_t kThreadPausedOffset = 0x71A;
    constexpr std::uint32_t kThreadPausableOffset = 0x71B;
    constexpr std::uint32_t kScriptHookThreadHash = 0x6758BF00u; // joaat("audiotest")
    constexpr std::uint32_t kMaxScriptThreads = 1024;

    constexpr std::uint32_t Joaat(const char* text)
    {
        std::uint32_t h = 0;
        for (; *text; ++text)
        {
            const char c = *text >= 'A' && *text <= 'Z' ? static_cast<char>(*text + 32) : *text;
            h += static_cast<std::uint8_t>(c);
            h += h << 10;
            h ^= h >> 6;
        }
        h += h << 3;
        h ^= h >> 11;
        h += h << 15;
        return h;
    }
    static_assert(Joaat("audiotest") == kScriptHookThreadHash, "ScriptHook's thread script hash");

    struct ThreadView
    {
        std::uint32_t id = 0;
        std::uint32_t scriptHash = 0;
        std::uint8_t paused = 0;
        std::uint8_t pausable = 0;
    };

    // A thread the native just paused that runs another ASI: its pause is
    // undone.
    inline bool UnpauseForScriptHook(const ThreadView& thread)
    {
        return thread.id != 0 && thread.scriptHash == kScriptHookThreadHash && thread.paused != 0;
    }
}
