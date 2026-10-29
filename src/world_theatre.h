#pragma once

// Closed theater with the engine's own functions. While the replay owns the
// world (WorldModel=gta, from the clear on the first playback until the save
// reload), the replay takes the world over like a replay editor does:
//   - persistent characters despawned and held (PERSCHAR native
//     0x92690B0822493CE0 -> 0xD1116C, flag [[0x4A35800]+0x10]);
//   - every vehicle removed with the vehicle reuse pool (RemoveAllVehs
//     0x134E95C(false), which ends with FlushVehicleReusePool 0x130DD84);
//   - every ped but the player removed (RemoveAllPedsWithException
//     0xC0F83C(player, 4 = ForceAllRandomAndMission, nullptr, true, true));
//   - the game's script threads paused every tick (_PAUSE_SCRIPT_THREADS),
//     so a thread launched after the clear sleeps too;
//   - ambient population denied at its single funnel CreatePopulationPed
//     0xBF23D4;
//   - the static emitters reset (StopAndResetStaticEmitters 0x312F7A4 on
//     0x5C505C0).
// Evidence (log only): every ped left after the clear and why, the spawn
// funnel's callers during the replay, the mix scenes started (0x2F19018) and
// the speech state 3 refusals (0x2F972C4).
//
// Each internal function is checked at its RVA (24 prologue bytes) and by
// the call or field sites the use relies on; a mismatch turns that feature
// off for the session and logs it. Addresses: build 1491.50.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace replay::theatre
{
    // ---- ini ([RedemptionEditor]) ------------------------------------------

    // Each step has its own key so the bench measures them one at a time.
    struct Config
    {
        bool log = true;          // TheatreLog: the census and the evidence lines
        bool perschar = true;     // TheatrePerschar
        bool vehicles = true;     // TheatreVehicles: RemoveAllVehs(false)
        bool peds = true;         // TheatrePeds: RemoveAllPedsWithException mode 4
        bool pauseScripts = true; // TheatrePauseScripts
        // TheatrePauseScriptHook=1 also pauses the threads ScriptHookRDR2
        // runs the other ASIs in (a camera mod, a trainer); 0 (default)
        // leaves them running.
        bool pauseScriptHook = false;
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

    struct Signature
    {
        const char* name;
        std::uint32_t rva;
        std::uint8_t bytes[24];
    };

    constexpr Signature kSpawnFunnelSignature{"CreatePopulationPed", 0xBF23D4,
        {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x18, 0x4C, 0x89, 0x70,
            0x20, 0x55, 0x48, 0x8D, 0x68, 0xF1}};
    constexpr Signature kMixSceneSignature{"MixerStartScene", 0x2F19018,
        {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x41, 0x56, 0x48,
            0x83, 0xEC, 0x40, 0x49, 0x8B, 0xF0}};
    constexpr Signature kRemoveAllVehsSignature{"RemoveAllVehs", 0x134E95C,
        {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78,
            0x20, 0x41, 0x56, 0x48, 0x83, 0xEC}};
    constexpr Signature kFlushVehicleReuseSignature{"FlushVehicleReusePool", 0x130DD84,
        {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83,
            0xEC, 0x20, 0x8B, 0x15, 0x02, 0x4D}};
    constexpr Signature kRemoveAllPedsSignature{"RemoveAllPedsWithException", 0xC0F83C,
        {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41, 0x54,
            0x41, 0x55, 0x41, 0x56, 0x41, 0x57}};
    constexpr Signature kStaticEmittersSignature{"StopAndResetStaticEmitters", 0x312F7A4,
        {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xF6, 0x48,
            0x8B, 0xF9, 0x39, 0xB1, 0xC0, 0xD0}};
    constexpr Signature kSpeechRulesSignature{"SpeechState3Rules", 0x2F972C4,
        {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83,
            0xEC, 0x20, 0xF6, 0x81, 0xE0, 0x03}};

    // The speech manager's per-owner release of the ambient slots (see
    // speech_slots.h; 0x2FB1E94(manager, speechEntity): every
    // ambient slot whose +0x28 is the entity goes through 0x2C50F3C, which
    // frees it only when each user of its reference says it may go,
    // 0x2C32D18). Its one caller is the speech entity's destructor 0x2F9316C.
    constexpr Signature kAmbientReleaseSignature{"FreeAmbientSlotsOfOwner", 0x2FB1E94,
        {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB, 0x48,
            0x8B, 0xF2, 0x48, 0x8B, 0xF9, 0x66}};
    // The speech manager's ambient slot array and count, as 0x2FB1E94 reads
    // them (+0x3F0, u16 at +0x3F8) and the stride (0x38).
    constexpr std::uint32_t kAmbientArray = 0x3F0;
    constexpr std::uint32_t kAmbientCount = 0x3F8;
    constexpr std::uint32_t kAmbientStride = 0x38;
    // The state 3 handler of a line (0x2FC486C, the
    // dispatcher 0x2FB4C23 calls it at 0x2FB4CAF and stops the line with
    // reason 6 when 0x2F49A64 says the code is a failure: below 0x46). Its
    // own failures: 0xB (no position, 0x2F9EEAC), 0xA (the data's play
    // 0x2FB26EC refused), or the rule code of speech entity vtable +0x68.
    // A log-only detour records the code (TheatreHooks bit 8).
    constexpr Signature kSpeechState3Signature{"SpeechState3Handler", 0x2FC486C,
        {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0x48, 0x81,
            0xEC, 0x90, 0x00, 0x00, 0x00, 0x48}};
    constexpr std::uint32_t kHookState3 = 8;
    // The mixer scene stop 0x327A07C(scene, byte) (see mix_scene_core.h),
    // 97 callers, STOP_AUDIO_SCENE's 0x31C946C among them;
    // the scene's hash is at +0x328 (written by the start 0x2F19018). A
    // log/record-only detour (TheatreHooks bit 16): with the start detour it
    // gives each scene's start and stop while SetSceneObserve is on.
    constexpr Signature kMixSceneStopSignature{"MixerStopScene", 0x327A07C,
        {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78,
            0x20, 0x41, 0x56, 0x48, 0x83, 0xEC}};
    constexpr std::uint32_t kHookSceneStop = 16;
    constexpr std::uint32_t kMixSceneHashOffset = 0x328;
    // The conversation scene's start (0x2FC2200) passes the conversation's
    // +0xE8 as the scene's out pointer; the participants are at +0x30 (array,
    // stride 0xA0) and +0x38 (u16 count), each with its tag at +0x7C and the
    // "has lines" byte at +0x98 (0x2FB6FED sets it, 0x2FC22B5 reads it).
    constexpr std::uint32_t kConversationSceneCaller = 0x2FC2335; // return address of the start at 0x2FC2330
    constexpr std::uint32_t kConversationSceneSlot = 0xE8;
    constexpr std::uint32_t kConversationParticipants = 0x30;
    constexpr std::uint32_t kConversationParticipantCount = 0x38;
    constexpr std::uint32_t kParticipantStride = 0xA0;
    constexpr std::uint32_t kParticipantTag = 0x7C;
    constexpr std::uint32_t kParticipantFlag = 0x98;
    constexpr std::uint32_t kParticipantReadMax = 32;
    // Codes of 0x2F49A64: 0x46 and above are success (0x49 to 0x4D seen).
    constexpr bool SpeechCodeFailed(std::uint32_t code) { return code < 0x46; }

    inline bool SignatureMatches(const Signature& signature, const std::uint8_t* code, std::size_t size)
    {
        return code && size >= sizeof(signature.bytes) && std::memcmp(code, signature.bytes, sizeof(signature.bytes)) == 0;
    }

    // A call/jmp rel32 (5 bytes) or a rip-relative instruction (7 bytes)
    // whose target is checked: it proves the function's tail or the global's
    // address in this build, not only a prologue.
    struct Site
    {
        const char* name;
        std::uint32_t rva;
        std::uint8_t bytes[7];
        std::uint32_t size; // 5 (E8/E9 rel32) or 7 (REX opcode ModRM disp32)
        std::uint32_t target;
    };

    // RemoveAllPedsWithException ends with FlushPedQueues(1, 1) and a jmp to
    // the deferred ped destruction 0x73381C (= 0xC1BB5C(-1)).
    constexpr Site kPedFlushSite{"peds_flush_queues", 0xC0FB67, {0xE8, 0x18, 0x41, 0xFF, 0xFF}, 5, 0xC03C84};
    constexpr Site kPedDestroySite{"peds_deferred_destroy", 0xC0FB88, {0xE9, 0x8F, 0x3C, 0xB2, 0xFF}, 5, 0x73381C};
    // RemoveAllVehs calls FlushVehicleReusePool before its own deferred pass.
    constexpr Site kVehicleReuseSite{"vehicles_reuse_pool", 0x134EA3D, {0xE8, 0x42, 0xF3, 0xFB, 0xFF}, 5, 0x130DD84};
    // The audio reset passes the static emitter manager by lea rcx.
    constexpr std::uint32_t kStaticEmitterManagerRva = 0x5C505C0;
    constexpr Site kEmitterManagerSite{"emitter_manager", 0x2F1928A, {0x48, 0x8D, 0x0D, 0x2F, 0x73, 0xD3, 0x02}, 7,
        kStaticEmitterManagerRva};
    // The PERSCHAR native's handler sets [[0x4A35800]+0x10] = 1 after 0xD1116C.
    constexpr std::uint32_t kPerscharManagerRva = 0x4A35800;
    constexpr std::uint32_t kPerscharHeldOffset = 0x10;
    constexpr Site kPerscharManagerSite{"perschar_manager", 0xFB391D, {0x48, 0x8B, 0x05, 0xDC, 0x1E, 0xA8, 0x03}, 7,
        kPerscharManagerRva};
    // The speech manager (0x5BB9750, passed by lea rcx to 0x2FB1E94 by the
    // speech entity's destructor at 0x2F93191).
    constexpr std::uint32_t kSpeechManagerRva = 0x5BB9750;
    constexpr Site kSpeechManagerSite{"speech_manager", 0x2F93191, {0x48, 0x8D, 0x0D, 0xB8, 0x65, 0xC2, 0x02}, 7,
        kSpeechManagerRva};

    constexpr std::uint32_t SiteTarget(const Site& site)
    {
        const std::uint32_t d = site.size - 4;
        const std::uint32_t value = static_cast<std::uint32_t>(site.bytes[d]) | (static_cast<std::uint32_t>(site.bytes[d + 1]) << 8) |
            (static_cast<std::uint32_t>(site.bytes[d + 2]) << 16) | (static_cast<std::uint32_t>(site.bytes[d + 3]) << 24);
        return site.rva + site.size + value;
    }

    inline bool SiteMatches(const Site& site, const std::uint8_t* code, std::size_t size)
    {
        return code && size >= site.size && std::memcmp(code, site.bytes, site.size) == 0;
    }

    // ---- the animal bank picker (animal_bank.h) ----------------------------
    // The type picker 0x2F985B0(manager) -> type (0: none), called once, by
    // the manager's animal bank update at 0x2FC6722 (only with no load
    // pending). The detour returns the held type or 0 (animal_bank.h
    // DecidePick); it reads the records and writes only the held type's next
    // bank number, inside the manager's own update, before 0x2FC67A4 reads it.
    constexpr Signature kAnimalBankPickSignature{"AnimalBankPick", 0x2F985B0,
        {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x57, 0x48, 0x83,
            0xEC, 0x30, 0x33, 0xDB, 0x0F, 0x29}};
    constexpr Site kAnimalBankPickSite{"animal_bank_pick_call", 0x2FC6722, {0xE8, 0x89, 0x1E, 0xFD, 0xFF}, 5, 0x2F985B0};
    // The loader's use of the record: the bank count (+0x8B8, 0x2FC6739)
    // before the slot finder, the next bank number (+0x8B4, 0x2FC67A4) given
    // to the name builder 0x2F9BD4C.
    constexpr Signature kAnimalBankCountSignature{"AnimalBankCount", 0x2FC6735,
        {0x4C, 0x8D, 0x3C, 0xF6, 0x46, 0x39, 0xA4, 0xFF, 0xB8, 0x08, 0x00, 0x00, 0x0F, 0x86, 0xD6, 0x00, 0x00, 0x00,
            0x8B, 0xD6, 0x48, 0x8B, 0xCF, 0xE8}};
    constexpr Signature kAnimalBankNextSignature{"AnimalBankNext", 0x2FC67A0,
        {0x48, 0x8D, 0x1C, 0xF6, 0x44, 0x8B, 0x84, 0xDF, 0xB4, 0x08, 0x00, 0x00, 0x4C, 0x8D, 0x4C, 0x24, 0x40, 0x48,
            0x89, 0x44, 0x24, 0x28, 0x8B, 0xD6}};
    // The animal voice of a type (0x2FA6674): the holder object (+0x8A8),
    // the holder (+0x898), then the entry's state 5 and its voice +0x30.
    constexpr Signature kAnimalVoiceHolderSignature{"AnimalVoiceHolder", 0x2FA668B,
        {0x48, 0x39, 0x9C, 0xD1, 0xA8, 0x08, 0x00, 0x00, 0x74, 0x2E, 0x48, 0x8D, 0x0C, 0xD1, 0x48, 0x81, 0xC1, 0x98,
            0x08, 0x00, 0x00, 0xE8, 0xA7, 0x34}};
    constexpr Signature kAnimalVoiceEntrySignature{"AnimalVoiceEntry", 0x2FA66AD,
        {0x83, 0x78, 0x20, 0x05, 0x75, 0x10, 0x48, 0x8B, 0x00, 0x48, 0x8B, 0xCF, 0xFF, 0x50, 0x20, 0x84, 0xC0, 0x74,
            0x03, 0x8B, 0x5F, 0x30, 0x8B, 0xC3}};
    // The release pass: the kept list +0x26D8 bounded by u16 [+0x848].
    constexpr Signature kAnimalBankKeepSignature{"AnimalBankKeep", 0x2FC6638,
        {0x44, 0x0F, 0xB7, 0xCA, 0x41, 0x8B, 0xCC, 0x45, 0x85, 0xC9, 0x74, 0x17, 0x48, 0x8D, 0x87, 0xD8, 0x26, 0x00,
            0x00, 0x44, 0x39, 0x00, 0x74, 0x1C}};
    // The name builder's type name table (.data, char* per type) and format.
    constexpr std::uint32_t kAnimalNameTableRva = 0x3E276A0;
    constexpr Site kAnimalNameTableSite{"animal_name_table", 0x2F9BD84, {0x48, 0x8D, 0x05, 0x15, 0xB9, 0xE8, 0x00}, 7,
        kAnimalNameTableRva};
    constexpr std::uint32_t kAnimalBankFormatRva = 0x37FEC88; // "%s_NEAR_%.2u"
    constexpr Site kAnimalBankFormatSite{"animal_bank_format", 0x2F9BD97, {0x4C, 0x8D, 0x05, 0xEA, 0x2E, 0x86, 0x00}, 7,
        kAnimalBankFormatRva};

    // RemoveAllPedsWithException's modes: 4 deletes every
    // ped but the player, the exception and its mount, clones and a class of
    // population type 1; the script extension is taken off first (0xEF6D34).
    constexpr int kRemovePedsForceAllRandomAndMission = 4;

    // Speech manager layout (offline, 0x2FBD044 and 0x2FBD440): two bank slot
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
    // _PAUSE_SCRIPT_THREADS(TRUE) (0x37C1257849DEF24A -> 0xF72354 -> 0xEED6B8)
    // sets the pause byte +0x71A of every thread with an id
    // (+0x8) but the calling one (the current thread, 0x2AFE944) and those
    // whose +0x71B is 0; the thread tick 0xEF73AC returns at once while
    // +0x71A is set. The replay's own thread is the caller and keeps running;
    // every other ASI's thread sleeps until the reload.
    //
    // While the replay pauses the game's scripts, the threads ScriptHook
    // started (script hash joaat("audiotest"), written by the thread reset
    // 0x2AFF76C into +0xC) get their pause byte back to 0 right after the
    // native, in the same tick (the unpause 0xEF6C84 writes the same byte):
    // the native's own effect undone for those threads only.
    constexpr Signature kPauseThreadsSignature{"PauseScriptThreads", 0xEED6B8,
        {0x44, 0x0F, 0xB7, 0x0D, 0x10, 0x53, 0xAC, 0x04, 0x45, 0x33, 0xDB, 0x4C, 0x8B, 0xD1, 0x45, 0x8B, 0xC3, 0x66,
            0x45, 0x3B, 0xD9, 0x73, 0x3D, 0x48}};
    // The pause loop's body: cmp byte [rcx+0x71B], r11b; je; mov byte [rcx+0x71A], 1.
    constexpr Signature kPauseThreadsMarkSignature{"PauseScriptThreadsMark", 0xEED6E8,
        {0x44, 0x38, 0x99, 0x1B, 0x07, 0x00, 0x00, 0x74, 0x0F, 0xC6, 0x81, 0x1A, 0x07, 0x00, 0x00, 0x01, 0x44, 0x0F,
            0xB7, 0x0D, 0xD0, 0x52, 0xAC, 0x04}};
    // The thread tick: cmp byte [rcx+0x71A], 0 at its top.
    constexpr Signature kThreadTickSignature{"GtaThreadTick", 0xEF73AC,
        {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9, 0x1A, 0x07, 0x00, 0x00, 0x00, 0x8B,
            0xFA, 0x48, 0x8B, 0xD9, 0x74, 0x05}};
    // The thread array (qword) and its count (word just after), read by the
    // pause loop: mov rax, [rip+disp] at 0xEED6CF; the count's movzx at
    // 0xEED6B8 is in the signature above (0xEED6B8 + 8 + 0x4AC5310).
    constexpr std::uint32_t kThreadArrayRva = 0x59B29C8;
    constexpr std::uint32_t kThreadCountRva = 0x59B29D0;
    constexpr Site kThreadArraySite{"script_threads", 0xEED6CF, {0x48, 0x8B, 0x05, 0xF2, 0x52, 0xAC, 0x04}, 7,
        kThreadArrayRva};
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
    // undone unless TheatrePauseScriptHook=1.
    inline bool UnpauseForScriptHook(const ThreadView& thread, bool pauseScriptHook)
    {
        return !pauseScriptHook && thread.id != 0 && thread.scriptHash == kScriptHookThreadHash && thread.paused != 0;
    }
}
