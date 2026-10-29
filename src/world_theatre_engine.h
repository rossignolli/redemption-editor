#pragma once

// Closed theater, the engine side (world_theatre.h has the pure parts and
// the addresses). Detours never call natives: they forward and record into
// rings the script thread reads. The engine calls run on the script thread
// only, under SEH, each after its signature and sites passed.

#include "animal_bank.h"
#include "world_theatre.h"

#include <cstdint>
#include <string>
#include <vector>

namespace replay::theatre
{
    // The three detours (spawn funnel, mix scene start, speech state 3
    // rules), each installed alone; a signature mismatch leaves that one off.
    void Install(std::uint32_t hooks, std::string& report);
    void Shutdown();

    // The replay owns the world (from the clear to the reload): the rings
    // record, and with `deny` the spawn funnel returns no ped.
    void SetOwning(bool owning, bool deny);
    bool Owning();
    bool SpawnFunnelInstalled();

    struct SpawnCall
    {
        std::uint64_t timeUs = 0;
        char label[48] = {};
        std::int32_t model = -1; // model index (not a hash)
        float position[3] = {};
        std::uint32_t caller = 0; // RVA of the return address
        bool denied = false;
    };
    void CollectSpawns(std::uint64_t& cursor, std::vector<SpawnCall>& out);

    struct SceneCall
    {
        std::uint64_t timeUs = 0;
        std::uint32_t hash = 0;
        std::uint32_t caller = 0;
    };
    void CollectScenes(std::uint64_t& cursor, std::vector<SceneCall>& out);

    // Every mixer scene start (after the original, with the scene it made)
    // and stop (before the original, with the scene's hash), whatever the
    // phase, while SetSceneObserve is on (MixSceneRecord, MixSceneReplay or
    // SpeechLevelProbe; mix_scene_core.h). A conversation scene's start also
    // carries its participants' tags (guarded reads).
    struct MixSceneEvent
    {
        std::uint64_t timeUs = 0;
        std::uint64_t scene = 0; // the scene object (0: the start made none)
        std::uint64_t slot = 0;  // start: where the engine keeps the scene pointer (r8)
        std::uint32_t hash = 0;
        std::uint32_t caller = 0; // start: RVA of the return address
        std::uint8_t stop = 0;
        std::uint8_t tagCount = 0;
        std::uint16_t participants = 0; // conversation: participants read
        std::uint32_t tags[8] = {};
    };
    void SetSceneObserve(bool on);
    void CollectMixSceneEvents(std::uint64_t& cursor, std::vector<MixSceneEvent>& out);
    // While the replay owns the world, the start detour refuses the starts
    // whose return address is one of these RVAs (the drivers of the scenes the
    // replay owns, mix_scene_pool_core.h kDrivers, refusable only where the
    // driver's handling of an empty reference is proven), the way a full pool
    // does: 0, the reference untouched, nothing started. Script thread sets (an
    // empty list ends it); the detour reads.
    constexpr std::size_t kMaxSceneRefusals = 8;
    void SetSceneRefusals(const std::vector<std::uint32_t>& returnRvas);
    std::uint64_t SceneRefusedCount();
    bool SceneStartInstalled();
    bool SceneStopInstalled();

    struct RuleRefusal
    {
        std::uint64_t timeUs = 0;
        std::uint64_t speechEntity = 0;
        std::uint64_t ped = 0; // speech entity +0x98
        std::uint32_t code = 0;
        std::uint8_t e3e0 = 0, b2 = 0, b3 = 0;
        std::uint32_t caller = 0;
    };
    void CollectRefusals(std::uint64_t& cursor, std::vector<RuleRefusal>& out);

    // The state 3 handler's failure codes (0x2FC486C, TheatreHooks bit 8),
    // recorded while SetSpeechObserve is on (the whole playback, the theater
    // owning the world or not: the mission lines).
    struct State3Failure
    {
        std::uint64_t timeUs = 0;
        std::uint64_t line = 0;
        std::uint32_t code = 0;
    };
    void SetSpeechObserve(bool on);
    void CollectState3Failures(std::uint64_t& cursor, std::vector<State3Failure>& out);
    bool State3HandlerInstalled();

    // A line the replay asked passes the rules' world context codes 28, 41 and
    // 48 while SetSpeechObserve is on (ini SpeechReplayRules, default 1; needs
    // the rules detour, TheatreHooks bit 4; speech_replay_rules.h). Script
    // thread: the lines created, cleared at each playback's start; the counts
    // per code since.
    void SetReplayLineRules(bool on);
    bool ReplayLineRulesReady(); // on and the rules detour installed
    void NoteReplayLine(std::uint64_t line);
    void ClearReplayLines();
    void ReplayRulesBypassed(std::uint32_t& otherSpeaker, std::uint32_t& outOfRange, std::uint32_t& conversation);


    // ---- engine calls (script thread) -----------------------------------

    enum class CallResult : std::uint8_t
    {
        Ok,
        Signature, // prologue or a site differs: the feature is off for the session
        Exception,
    };
    const char* CallResultName(CallResult result);

    // Checked once per session: every signature and site the calls use.
    // `failed` names the first one that did not match ("none").
    struct Checks
    {
        bool vehicles = false;
        bool peds = false;
        bool emitters = false;
        bool perschar = false; // the flag read only
        bool speech = false;   // the manager read only
        std::string failed = "none";
    };
    const Checks& RunChecks();

    CallResult RemoveAllVehicles(std::uint32_t& code);
    CallResult RemoveAllPeds(std::uint64_t playerPed, std::uint32_t& code);
    CallResult ResetStaticEmitters(std::uint32_t& code);
    // [[0x4A35800]+0x10]: -1 when unreadable.
    int PerscharHeld();

    // The speech manager's slots: used/total per class, and the ambient slots
    // held by a puppet (one of `puppetPeds`) or by anything else.
    struct SlotCensus
    {
        bool ok = false;
        std::uint32_t used[4] = {};
        std::uint32_t total[4] = {};
        std::uint32_t ambientByPuppet = 0;
        std::uint32_t ambientByWorld = 0;
    };
    SlotCensus ReadSpeechSlots(const std::uint64_t* puppetPeds, std::size_t count);
    // The bank slot whose wave slot is `waveSlot`: class index (0 to 2) and
    // bank, false when none holds it.
    bool BankOfWaveSlot(std::uint64_t waveSlot, int& slotClass, std::uint32_t& bank);

    // Right after _PAUSE_SCRIPT_THREADS(TRUE), the threads ScriptHookRDR2
    // started for the other ASIs get their pause byte back (unless
    // `pauseScriptHook`). Checked once (signatures and the thread array site);
    // `census` (may be null) receives every thread seen.
    struct ThreadSpare
    {
        bool ok = false;              // checks passed and the array was read
        std::string error = "none";   // the first check that failed, or the read
        std::uint32_t threads = 0;    // threads with an id
        std::uint32_t pausedByNative = 0;
        std::uint32_t scriptHook = 0; // threads with ScriptHook's script hash
        std::uint32_t unpaused = 0;   // pause bytes put back to 0
        std::uint32_t writeFailed = 0;
    };
    ThreadSpare SpareScriptHookThreads(bool pauseScriptHook, std::vector<ThreadView>* census);

    // The speech manager's ambient (speech_slots.h) slots (guarded reads, rung
    // 2): per slot the live reference (+0x10, 0 = free), the wave slot (+0x18),
    // the owner speech entity (+0x28) and its ped (+0x98). `ok` false:
    // unreadable (the gate then lets lines go).
    struct AmbientSlot
    {
        std::uint64_t ref = 0;
        std::uint64_t wave = 0;
        std::uint64_t owner = 0;
        std::uint64_t ownerPed = 0;
        float priority = 0.0f;
        bool Free() const { return !ref && wave; }
    };
    struct AmbientSlots
    {
        bool ok = false;
        std::vector<AmbientSlot> slots;
        int FreeCount() const
        {
            int free = 0;
            for (const AmbientSlot& slot : slots)
                free += slot.Free() ? 1 : 0;
            return free;
        }
    };
    void ReadAmbientSlots(AmbientSlots& out);
    // 0x2FB1E94(manager, speechEntity), signature checked once, SEH (rung 4).
    CallResult ReleaseAmbientSlotsOf(std::uint64_t speechEntity, std::uint32_t& code);

    // ---- the animal banks (animal_bank.h) ------------------------------------
    // The type picker detour 0x2F985B0, installed alone after every check of
    // the records passed (signatures, sites, the name format); the report
    // gets "AnimalBankPick=ok;" or the reason. With no hold published it
    // only forwards.
    void InstallAnimalBank(std::string& report);
    bool AnimalBankInstalled();
    // The checks the reads and the detour rely on, once per session; the
    // first one that failed is named ("none").
    bool AnimalBankChecks(std::string& failed);
    // Every type with banks: its name from the engine's table and its bank
    // count (guarded reads; false when the checks failed).
    bool ReadAnimalBankTypes(std::vector<replay::animalbank::TypeBanks>& out);
    // A type's record now: the voice of the bank its holder has (0: none),
    // the same only while the entry is in state 5 (what a line by context
    // reads), the demand, the next bank number, the count, whether the
    // release pass keeps it and the exhausted flag.
    struct AnimalBankState
    {
        bool ok = false;
        std::uint32_t holderVoice = 0;
        std::uint32_t loadedVoice = 0;
        std::uint32_t entryState = 0;
        float demand = 0.0f;
        std::uint32_t next = 0;
        std::uint32_t count = 0;
        bool kept = false;
        bool exhausted = false;
    };
    AnimalBankState ReadAnimalBank(int type);
    // The holds the detour acts on (at most animalbank::kMaxHolds; empty
    // turns it into a plain forward). Script thread.
    void SetAnimalBankHolds(const std::vector<replay::animalbank::Hold>& holds);
    struct AnimalBankCounters
    {
        std::uint64_t picks = 0;     // picker calls with holds published
        std::uint64_t forced = 0;    // a held type returned with its bank number set
        std::uint64_t keeps = 0;     // a rotation of a type holding its voice refused (0 returned)
        std::uint64_t notKept = 0;   // calls where a held type was missing its bank but the manager would not keep it
        std::uint64_t writeFailed = 0;
        std::uint64_t otherManager = 0; // picker calls for another manager than 0x5BB9750 (only forwarded)
        std::uint32_t lastForcedType = 0;
        std::uint32_t lastForcedBank = 0;
    };
    AnimalBankCounters ReadAnimalBankCounters();
    void ResetAnimalBankCounters();
}
