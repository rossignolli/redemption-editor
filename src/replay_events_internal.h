// replay_events_internal.h: the state and helpers the replay_events*.cpp files share.
// Internal to those files, not a public header.
#pragma once

#include "replay_events.h"
#include "clip_sites.h" // the fire and weapon fx sites
#include "engine_sigs.h"
#include "world_theatre_engine.h" // closed theater (speech slot evidence)
#include "animal_bank.h" // the recorded voice's bank of an animal, loaded and held
#include "mod_identity.h" // RedemptionEditor.ini (ReplayProbe.ini is read too)

#include "audio_observer.h"
#include "burn_level.h"
#include "speech_identity.h"
#include "speech_record_match.h" // speech start matched by identity, reused addresses
#include "speech_resume.h" // speech said again after a jump, with a start offset
#include "speech_slots.h" // the replay manages the ambient speech slots
#include "speech_player_route.h" // the player's lines on the hidden player only at his puppet
#include "entity_events.h"
#include "decal_observer.h"
#include "decal_state.h"
#include "fx_observer.h"
#include "fx_check_core.h"
#include "ped_gore.h"
#include "ped_blood.h"
#include "fire_observer.h"
#include "fire_initial.h"
#include "replay_objects.h"
#include "replay_trains.h"
#include "pose_capture.h"
#include "pose_playback.h"
#include "profiler.h"
#include "entity_cache.h"
#include "weapon_observer.h"
#include "sound_census.h"
#include "mix_probe.h"
#include "render_target.h"
#include "game_listener.h"
#include "mix_scene.h" // mixer scenes and mix groups, recorded and replayed
#include "speech_level_probe.h" // the engine's volume of each speech line
#include "volume_probe.h" // the engine's gain of every voice, by stage
#include "world_sound_probe.h" // particle audio slots and horse group audio, read only
#include "wind_replay.h"
#include "category_state.h"
#include "speech_duck.h"
#include "speech_script_path.h" // scripted lines on their own path
#include "speech_conversation.h" // the recorded sound set and duck byte
#include "player_weapon_bank_core.h"
#include "version.generated.h"
#include "sound_generic.h"
#include "soundfx_probe.h"
#include "capture_probe.h"
#include "sound_slot_policy.h"
#include "sound_capture.h"
#include "state_budget.h"
#include "log_format.h"
#include "log_policy.h"
#include "region_cache.h"
#include "clip_pointer_audit.h"
#include "deferred_log.h"
#include "mod_clock.h"
#include "mount_audio.h"
#include "horse_audio_session.h"
#include "replay_playback.h"
#include "clip_area.h"
#include "registered_fx_rule.h"
#include "fx_motion_rule.h"
#include "script_fx_loop.h"
#include "shot_fx_audit_core.h"
#include "weapon_fx_intent_core.h"
#include "guarded_memory.h" // the muzzle info map and the viewport
#include "vegetation_speed.h"
#include "puppet_audio_rule.h"
#include "horse_sound_report.h"
#include "entity_sound_tally.h"
#include "melee_sound_check.h"
#include "entity_query.h"
#include "entity_query_track.h"
#include "ped_audio_state.h"
#include "ped_speed_core.h"
#include "foot_event_core.h"
#include "game_sig.h"
#include "shot_audio_delay.h"

#include "main.h"
#include "natives.h"

#include <algorithm>
#include <deque>
#include <ctime>
#include <fstream>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace replay::events
{

    namespace sig = replay::sig;
    using enginesig::kAudioFootHelperA;
    using enginesig::kAudioFootHelperB;
    using enginesig::kAudioStepCreate;
    using enginesig::kAudioStepLayers;
    using enginesig::kAudioStepOther;
    using enginesig::kAudioStepSound;
        Context& Ctx();
    extern bool g_poseV2ResearchWindows;
    extern bool g_poseV2AudioObserverReady;
    void OpenPoseV2AudioWindow(const std::string& label, std::uint64_t beforeUs, std::uint64_t afterUs);
    // The engine particle triggers around each recorded shot, logged 300 ms
    // after it so the whole window is in.
    struct PoseV2FxWindow
    {
        std::size_t shot = 0;
        std::uint64_t shotUs = 0;
        // Engine addresses at the shot, to locate the entity references in
        // the recorded fire event.
        std::uint64_t shooter = 0;
        std::uint64_t shooterAudio = 0;
        std::uint64_t weapon = 0;
        // The shot's own fire event, when the shot came from the queue.
        replay::audio::FireCall fire{};
        bool hasFire = false;
    };
    extern std::vector<PoseV2FxWindow> g_poseV2FxWindows;
    extern bool g_poseV2FxObserverReady;
    extern bool g_poseV2FxReplayEnabled;
    extern bool g_poseV2DecalObserverReady;
    extern std::uint64_t g_poseV2DecalCursor;
    extern std::uint64_t g_poseV2DecalsRefused;
    extern std::uint64_t g_poseV2DecalsInternal;
    extern std::uint64_t g_poseV2DecalsAttached;
    extern std::uint32_t g_poseV2DecalSamples;
    extern replay::entity::ForwardEventCursor g_poseV2DecalCursorPlay;
    extern std::uint64_t g_poseV2DecalsPlayed;
    extern std::uint64_t g_poseV2DecalsSkippedInternal;
    extern std::uint64_t g_poseV2DecalsNoEntity;
    extern std::uint64_t g_poseV2DecalsInfoUnknown;
    extern std::uint64_t g_poseV2DecalsNoInfo;
    extern std::uint64_t g_poseV2DecalsNullInfo;
    extern std::uint32_t g_poseV2DecalNullInfoLogs;
    extern std::uint64_t g_poseV2DecalsDeadIdReadd;
    extern std::uint32_t g_poseV2DecalGuardLogs;
    extern std::uint64_t g_poseV2DecalsFailed;
    extern std::string g_poseV2DecalLastError;
    extern std::uint64_t g_poseV2DecalRemoveCursor;
    extern replay::entity::ForwardEventCursor g_poseV2DecalRemoveCursorPlay;
    extern std::map<std::uint32_t, std::uint32_t> g_poseV2DecalIds;
    extern std::uint64_t g_poseV2DecalsRemoved;
    extern std::uint64_t g_poseV2DecalsReplaced;
    extern bool g_poseV2BloodObserverReady;
    extern bool g_poseV2FireReady;
    extern std::map<std::int32_t, std::uint32_t> g_poseV2FireActiveSlots;
    extern std::uint64_t g_poseV2FireCharShielded;
    struct OurFire
    {
        std::uint32_t start = 0;
        void* fire = nullptr;
        bool alive = false;
        // After a jump: the ground state at restoreMs is written on the
        // fire's area item once the engine made it; endAfter: the fire had
        // ended by then and is shut down right after the write.
        bool restore = false;
        bool endAfter = false;
        bool stopped = false; // its recorded stop was played
        std::uint8_t endInstant = 0;
        std::uint8_t endArg3 = 0;
        std::uint32_t restoreMs = 0;
        ULONGLONG restoreSince = 0;
        // The slot generation of this start; every write into the CFire goes
        // through WriteOurFire with it.
        std::uint32_t generation = 0;
    };
    extern std::map<std::uint64_t, std::uint32_t> g_poseV2FireOwners;
    extern std::uint32_t g_poseV2FireItemSampleAt;
    extern std::uint64_t g_poseV2FireRestored;
    extern std::uint64_t g_poseV2FireRestoreMissed;
    extern std::vector<OurFire> g_poseV2FiresOurs;
    extern replay::entity::ForwardEventCursor g_poseV2FireStartCursorPlay;
    extern replay::entity::ForwardEventCursor g_poseV2FireStopCursorPlay;
    extern std::uint32_t g_poseV2FireLastPlayhead;
    extern std::uint64_t g_poseV2FireRebuilt;
    extern std::uint64_t g_poseV2FireUntrackedSkipped;
    extern std::uint64_t g_poseV2FirePtfxOwned;
    extern std::uint32_t g_poseV2FireUntrackedLogs;
    extern std::uint8_t g_poseV2FireUntrackedSeen[replay::fire::kFireCount];
    extern std::set<std::uint32_t> g_poseV2HeldStateTaken;
    extern std::uint64_t g_poseV2HeldBlood;
    extern std::uint64_t g_poseV2HeldGoreBits;
    extern std::uint64_t g_poseV2HeldPieces;
    extern std::uint64_t g_poseV2FirePlayCursor;
    extern std::uint64_t g_poseV2FirePlayed;
    extern std::uint64_t g_poseV2FirePlayRefused;
    extern std::uint64_t g_poseV2FireStopped;
    extern std::uint64_t g_poseV2FireEngineEnded;
    extern std::uint32_t g_poseV2FireEntityLogs;
    extern std::uint32_t g_poseV2FireEntityNextMs;
    extern std::uint64_t g_poseV2FireReconciles;
    extern std::uint64_t g_poseV2FireRemnantsCleared;
    extern std::string g_poseV2FireLastError;
    extern std::vector<replay::fire::StartRetry> g_poseV2FireRetry;
    extern std::vector<std::uint32_t> g_poseV2FirePending;
    extern std::uint64_t g_poseV2FireEntityWaits;
    extern std::uint64_t g_poseV2FireRetries;
    extern std::uint64_t g_poseV2FireGaveUp;
    extern std::uint64_t g_poseV2FireParentCleared;
    extern std::uint64_t g_poseV2FireIncremental;
    extern std::vector<std::uint32_t> g_poseV2FireEndedQueue;
    extern std::uint64_t g_poseV2FireDuplicates;
    extern std::uint32_t g_poseV2FireHoldLastMs;
    extern std::uint64_t g_poseV2FireEndedQueued;
    extern std::vector<std::uint32_t> g_poseV2FireFirstSampleMs;
    extern std::uint32_t g_poseV2FireSlotLogs;
    extern std::uint32_t g_poseV2FireSkipLogs;
    extern std::uint32_t g_poseV2FireReconcileLogs;
    extern bool g_poseV2FirePlaying;
    extern float g_poseV2FireCenter[3];
    extern float g_poseV2FireRadius;
    extern ULONGLONG g_poseV2FireCheckAt;
    std::uint32_t CallReturnRva(const sig::Signature& call);
    extern std::uint64_t g_poseV2BloodCursor;
    extern std::uint64_t g_poseV2BloodUnmapped;
    extern std::uint64_t g_poseV2BloodNotCopied;
    extern std::uint64_t g_poseV2BloodFromCache;
    extern std::uint64_t g_poseV2BloodNotPed;
    extern std::uint32_t g_poseV2BloodUnmappedLogs;
    extern std::vector<std::pair<std::uint64_t, replay::entity::ReplayId>> g_poseV2BloodOwnerCache;
    extern std::map<std::uint32_t, std::uint32_t> g_poseV2BloodByCaller;
    extern std::uint32_t g_poseV2BloodLogged;
    extern std::uint64_t g_poseV2ProjFromBlood;
    extern replay::entity::ForwardEventCursor g_poseV2BloodCursorPlay;
    extern std::uint32_t g_poseV2BloodLastPlayhead;
    extern std::map<std::uint32_t, std::uint32_t> g_poseV2BloodAppliedUpTo;
    extern std::map<std::uint32_t, int> g_poseV2BloodAppliedClone;
    extern std::vector<std::size_t> g_poseV2BloodPending;
    extern std::uint64_t g_poseV2BloodPlayed;
    extern std::uint64_t g_poseV2BloodFailed;
    extern std::uint64_t g_poseV2BloodPresent;
    extern std::uint64_t g_poseV2BloodBusy;
    extern std::uint64_t g_poseV2BloodReconciles;
    extern std::string g_poseV2BloodLastError;
    extern replay::budget::OpQueue g_poseV2BloodQueue;
    extern std::uint32_t g_poseV2StateBudgetUs;
    extern std::uint32_t g_poseV2BloodQueueLogs;
    extern std::uint32_t g_poseV2BloodSlowLogs;
    extern std::uint32_t g_poseV2BloodTickLogs;
    extern std::uint64_t g_poseV2GoreHitCursor;
    extern std::uint64_t g_poseV2GoreHitsOther;
    extern replay::entity::ForwardEventCursor g_poseV2GoreHitCursorPlay;
    extern std::uint64_t g_poseV2GoreHitsPlayed;
    extern std::uint64_t g_poseV2GoreHitsFailed;
    extern std::uint64_t g_poseV2GoreHitsNoPuppet;
    extern std::uint64_t g_poseV2GoreMaskSkipped;
    extern std::uint32_t g_poseV2GorePieceUntilMs;
    extern Vector3 g_poseV2GorePieceCenter;
    extern std::uint32_t g_poseV2GorePieceModel;
    extern Entity g_poseV2GorePieceOwner;
    extern replay::entity::ForwardEventCursor g_poseV2GoreCursorPlay;
    extern std::uint64_t g_poseV2GorePlayed;
    extern std::uint64_t g_poseV2GoreFailed;
    extern std::uint64_t g_poseV2GoreNoPuppet;
    extern std::string g_poseV2GoreLastError;
    extern std::uint64_t g_poseV2ProjCursor;
    extern std::uint32_t g_poseV2ProjSamples;
    extern std::uint64_t g_poseV2ProjNoEntity;
    extern replay::entity::ForwardEventCursor g_poseV2ProjCursorPlay;
    extern std::uint64_t g_poseV2ProjPlayed;
    extern std::uint64_t g_poseV2ProjFailed;
    extern std::uint64_t g_poseV2ProjMissing;
    extern std::uint64_t g_poseV2ProjNoInfo;
    extern std::string g_poseV2ProjLastError;
    extern std::uint64_t g_poseV2ProjFromRage;
    extern std::uint32_t g_poseV2ProjFromRageLogs;
    extern std::uint64_t g_poseV2ProjNullOwner;
    extern std::uint64_t g_poseV2ProjEngineCursor;
    extern std::uint64_t g_poseV2ProjEngineSeen;
    extern std::uint64_t g_poseV2ProjEngineMirrors;
    extern std::uint32_t g_poseV2ProjEngineLogs;
    extern std::uint64_t g_poseV2DecalsWithEntity;
    extern std::uint64_t g_poseV2DecalsEntityPassed;
    extern std::uint64_t g_poseV2DecalsEntityMissing;
    extern std::uint32_t g_poseV2DecalEntityLogs;
    extern std::uint32_t g_poseV2PoolSamples;
    extern std::uint64_t g_poseV2DecalsAgedInitial;
    extern std::uint64_t g_poseV2DecalsExpiredInitial;
    extern std::uint64_t g_poseV2DecalsAgedPlay;
    extern std::uint64_t g_poseV2DecalsExpiredPlay;
    extern std::uint32_t g_poseV2DecalAgeLogs;
    extern std::vector<std::uint32_t> g_poseV2RecordedAliveIds;
    extern std::set<std::uint32_t> g_poseV2RecordedAliveSet;
    extern std::size_t g_poseV2RecordedAliveAt;
    extern std::uint64_t g_poseV2DecalsExpired;
    extern std::uint32_t g_poseV2DecalExpiredLogs;
    extern std::size_t g_poseV2EngineFxOwnerCount;
    extern std::uint64_t g_poseV2BloodEvolved;
    extern std::uint64_t g_poseV2BloodEvolveFailed;
    extern std::uint64_t g_poseV2BloodEvolvePresent;
    extern std::uint32_t g_poseV2BloodEvolveLogs;
    extern std::uint64_t g_poseV2DecalRemovesUnmapped;
    extern std::uint64_t g_poseV2FxPhaseStartUs;
    extern replay::fxcheck::Tally g_fxCheck;
    extern std::size_t g_fxCheckRecordedEvents;
    extern std::size_t g_fxCheckRecordedTracks;
    extern std::uint64_t g_speechOnPlayer;
    extern std::uint64_t g_speechPlayerFar;
    extern std::uint32_t g_speechPlayerFarLogs;
    extern bool g_playerFollowMoving;
    extern ULONGLONG g_playerFollowAtMs;
    extern Ped g_speechHoldPuppet;
    extern Ped g_speechHoldPlayer;
    extern std::uint64_t g_speechHoldSinceMs;
    extern bool g_speechHoldLineAlive;
    extern std::uint64_t g_speechHeld;
    extern std::uint64_t g_speechHeldStarted;
    extern std::uint32_t g_speechHeldLogs;
    extern std::uint64_t g_speechCharacterMismatch;
    extern std::uint32_t g_speechCharacterMismatchLogs;
    void ReleasePlayerSpeechHold();
    extern std::uint64_t g_speechPlayerRefused;
    extern std::uint64_t g_speechPlayerRefusedPuppetCreated;
    extern std::uint32_t g_speechPlayerRefusedLogs;
    extern std::uint64_t g_speechPlayerNamedVoice;
    extern std::uint64_t g_speechPlayerNamedVoiceCreated;
    extern std::uint32_t g_speechPlayerNamedVoiceLogs;
    extern std::uint64_t g_speechConversationPlainRetries;
    extern std::uint64_t g_speechConversationPlainCreated;
    extern std::uint32_t g_speechConversationPlainLogs;
    extern std::uint32_t g_speechPlayerPacked;
    extern bool g_speechConversationLive;
    extern replay::speech::conversation::Tally g_speechConversationTally;
    extern std::uint32_t g_speechConversationLogs;
    extern replay::events::FxParticlesConfig g_fxCfg;
    extern std::uint32_t g_fxSpanId;
    extern std::vector<std::uint32_t> g_fxTrackCreatedSpan;
    extern std::vector<std::uint32_t> g_fxTrackPlaySpan;
    extern std::vector<replay::scriptfx::LoopTrackPlay> g_scriptLoopPlay;
    bool ScriptLoopSite(std::uint32_t site);
    extern std::uint32_t g_fxBlinkLogsByHash[4];
    extern std::vector<std::vector<std::uint8_t>> g_fxGapSeen;
    extern std::uint64_t g_fxGapCreates;
    extern std::vector<replay::weaponfx::ShotRow> g_fxShotRows;
    extern bool g_fxWeaponIntentReady;
    extern bool g_wfxPlayOn;
    extern std::vector<std::uint8_t> g_wfxDropTrack;
    extern std::uint64_t g_wfxDroppedTracksPlayed;
    extern std::uint32_t g_fxBlinkHashes[4];
    extern std::vector<std::vector<replay::entity::RegisteredFxGap>> g_fxDerivedGaps;
    extern std::uint32_t g_fxRecreateLogs;
    extern std::uint64_t g_fxRecreateInSpan;
    extern std::uint64_t g_fxRecreateReentered;
    extern std::uint64_t g_fxRecreateNotReady;
    extern std::uint64_t g_fxOwnerNotReadyTicks;
    extern std::uint64_t g_fxBackoffTicks;
    extern std::uint64_t g_fxGapTicks;
    extern std::uint64_t g_fxGapsEntered;
    extern std::uint64_t g_fxSettingsApplied;
    extern std::uint64_t g_fxSettingsFailed;
    extern std::uint64_t g_fxSettingsLegacyHoof;
    extern std::string g_fxSettingsError;
    extern std::uint32_t g_fxSettingsDiffLogs;
    extern std::uint64_t g_fxParentOnceSkipped;
    extern std::uint64_t g_fxBlinks;
    extern std::uint32_t g_fxBlinkLogs;
    extern std::uint64_t g_fxPlayRestarts;
    extern std::uint32_t g_fxRelocateProbeLogs;
    extern std::uint32_t g_fxVeladdProbeLogs;
    extern std::uint64_t g_fxRelocateCleared;
    extern std::uint64_t g_fxRelocateKept;
    extern std::uint64_t g_fxRelocateCalls;
    extern std::uint64_t g_fxRelocateFailed;
    extern std::uint64_t g_fxRelocateStale;
    extern std::uint64_t g_fxGapEndSkipped;
    extern std::string g_fxRelocateError;
    extern bool g_fxVelocityReady;
    extern bool g_fxPauseFreezeReady;
    extern std::uint64_t g_fxWorldOwnerRefusals;
    extern std::uint64_t g_fxOwnerListImmediate;
    extern std::size_t g_fxLastPuppetClones;
    extern std::vector<std::uint64_t> g_fxWorldOwnersLive;
    // Our instances on each puppet (rebuilt at the end of the events), the
    // placements of this tick and the probe's bits before the placement.
    struct FxOwned
    {
        std::uint64_t owner = 0;
        std::uint64_t inst = 0;
        std::uint32_t hash = 0;
        std::uint32_t lastFlags2 = 0;   // after the last end-of-tick clear
        bool lastKnown = false;
        Entity handle = 0;              // the owner's handle (its position at the end of the tick)
    };
    extern std::vector<FxOwned> g_fxOwned;
    // FxRelocateClearStill: the owners' positions at the end of the last
    // tick (an owner the playback did not place this tick is cleared by its
    // own step), the counters and the probe FX_RELOCATE_UNPLACED (capped per
    // hash).
    struct FxOwnerAt
    {
        float at[3] = {};
        std::uint64_t tick = 0;
    };
    extern std::unordered_map<std::uint64_t, FxOwnerAt> g_fxOwnerLastAt;
    extern std::uint64_t g_fxRelocateClearedStill;
    extern std::uint64_t g_fxRelocateKeptUnplaced;
    extern std::uint32_t g_fxUnplacedLogs;
    extern std::map<std::uint32_t, std::uint32_t> g_fxUnplacedLogsByHash;
    extern std::vector<std::pair<std::uint64_t, float>> g_fxPlacedThisTick;
    struct FxProbeBefore
    {
        std::uint64_t inst = 0;
        std::uint32_t before = 0;
        std::uint32_t prevTick = 0;
        bool prevKnown = false;
    };
    extern std::vector<FxProbeBefore> g_fxProbeBefore;
    // Our one-shots checked one tick after their start.
    struct FxPendingCheck
    {
        std::uint64_t inst = 0;
        std::uint32_t hash = 0;
        std::uint32_t site = 0;
        std::uint64_t entity = 0;     // attached: the puppet it rides
        float expected[3] = {};       // world: the recorded place
        bool weapon = false;
        bool hasSettings = false;
        replay::entity::FxSettings settings;
        std::uint32_t ticks = 0;
    };
    extern std::vector<FxPendingCheck> g_fxPendingChecks;
    extern bool g_fxClipWeaponEvents;
    extern std::vector<replay::shotfx::Decision> g_fxShotDecisions;
    extern std::uint32_t g_fxImpactOnBodyLogs;
    extern std::uint64_t g_fxShotResolveShots;
    extern std::uint64_t g_fxShotMuzzleStarts;
    extern std::uint64_t g_fxShotImpactStarts;
    extern std::uint64_t g_fxShotBodyStarts;
    extern std::uint64_t g_fxShotClaimedMulti;
    extern std::uint64_t g_fxShotNoMuzzle;
    extern std::uint64_t g_fxShotClaimed;
    extern std::map<std::uint64_t, std::uint32_t> g_fxShotTriggerClaims;
    extern std::uint32_t g_poseV2RegisteredPrevTickMs;
    // Why a registered track was not created (the last reason seen), for
    // the tracks the playback never created: the fx check's missing rows.
    enum class RegNotCreated : std::uint8_t { None, Created, FireSite, CameraAmbient, Paused, OwnerMissing, OwnerMismatch, OwnerNotReady, WeaponIntent };
    extern std::vector<RegNotCreated> g_fxRegReason;
    extern std::uint64_t g_fxRegRecreated;
    void LogPoseV2FxHistogram(const char* phase);
    extern bool g_poseV2FxPlayerReady;
    constexpr std::uint64_t kPoseV2FxAfterUs = 100000;
    extern replay::entity::ForwardEventCursor g_poseV2SpeechCursorPlay;
    extern std::uint64_t g_poseV2SpeechPlayed;

    // A queued line (recorded or replayed), followed every frame until it
    // leaves the speaker's line list: how long it lived, its states and why
    // it was stopped.
    struct PoseV2QueuedLine
    {
        Ped ped = 0;
        replay::entity::ReplayId id;
        std::uint64_t line = 0;
        std::uint64_t queuedUs = 0;
        std::uint32_t context = 0;
        std::uint32_t frames = 0;
        const char* phase = "playback";
        // Line state (+0x234) each time it changed, as "frame:state".
        std::string states;
        std::int32_t lastState = -2;
    };
    extern std::vector<PoseV2QueuedLine> g_poseV2QueuedLines;
    extern int g_poseV2QueuedLineBudget;
    extern std::uint64_t g_poseV2CoreCursor;
    extern std::uint64_t g_poseV2SpeechCancelled;
    extern replay::entity::ForwardEventCursor g_poseV2ShotCursor;
    extern std::uint64_t g_poseV2ShotsPlayed;
    extern std::uint64_t g_poseV2ShotsCancelled;
    replay::pose::Vec3 ToPoseVec(const Vector3& v);
    void BeginShotAudioDelay(const char* phase);
    void NoteShotAudioFire(const replay::entity::ReplayId& shooter, const std::uint8_t* info, std::uint64_t fireUs,
        std::uint32_t eventMs, const replay::pose::Vec3& muzzle, bool player);
    void TickShotAudioDelay();
    void EndShotAudioDelay(const std::map<std::uint32_t, replay::shotdelay::Reference>* refs);
    std::map<std::uint32_t, replay::shotdelay::Reference> ShotAudioClipReference();
    void LogShotGunfight(const char* phase, const replay::entity::ReplayId& shooter, void* ped, std::uint32_t timeMs);
    void MeasurePoseV2ImpactNormal(replay::entity::ShotEvent& shot, Ped shooter);
    extern std::uint64_t g_poseV2FootstepCursor;
    extern std::uint64_t g_poseV2ExplosionCursor;
    extern std::uint64_t g_poseV2ExplosionsPlayed;
    extern std::uint64_t g_poseV2ExplosionsRefused;
    extern std::string g_poseV2ExplosionLastError;
    extern std::uint64_t g_poseV2FootstepsUnrecorded;
    extern std::uint64_t g_poseV2FootMaterialCursor;
    extern replay::footmaterial::Pairer g_poseV2FootMaterialPairer;
    extern std::uint64_t g_poseV2FootMaterialPaired;
    extern std::uint64_t g_poseV2FootMaterialInvalid;
    extern std::uint64_t g_poseV2FootMaterialLost;
    extern std::uint64_t g_poseV2FxStartCursor;
    extern std::uint64_t g_poseV2FxSkippedShot;
    extern std::uint64_t g_poseV2FxSkippedEntity;
    extern std::uint64_t g_poseV2FxNoHash;
    extern std::uint64_t g_poseV2FxPlayed;
    extern std::uint64_t g_poseV2FxNoPuppet;
    extern std::uint64_t g_poseV2FxNoWorldPlay;
    extern std::uint64_t g_poseV2FxFailed;
    extern std::string g_poseV2FxLastError;
    extern replay::entity::ForwardEventCursor g_poseV2FxCursorPlay;
    extern std::map<std::uint32_t, std::uint32_t> g_poseV2FxRecordedByHash;
    extern std::map<std::uint32_t, std::uint32_t> g_poseV2FxSkippedByHash;
    extern std::map<std::uint32_t, std::uint32_t> g_poseV2FxNoHashByCaller;
    extern std::map<std::uint32_t, std::uint32_t> g_poseV2FxStartsByCaller;
    extern std::uint32_t g_poseV2FxSamples;
    extern std::uint64_t g_poseV2FxRegisteredCursor;
    struct RegisteredStat
    {
        std::uint32_t calls = 0;
        std::uint32_t created = 0;
        std::set<std::uint64_t> keys;
        std::uint32_t caller = 0;
    };
    extern std::map<std::uint32_t, RegisteredStat> g_poseV2FxRegistered;
    extern std::uint32_t g_poseV2FxRegisteredSamples;
    extern std::uint32_t g_poseV2FxWatchHash;
    void LogPoseV2FxSnapshot(const char* source, const std::vector<std::uint8_t>& bytes);

    // Registered fx track: live keys of the recording and the playback side.
    struct LiveRegistered
    {
        std::uint64_t key = 0;
        std::uint64_t inst = 0;
        std::size_t track = 0;      // index in clip.registeredFx
        std::uint32_t lastCallMs = 0;
        bool touched = false;
        // The engine entity the instance is attached to now (its address and
        // bone, as the attach observer saw them), whether it maps to a
        // recorded id yet, and since when it is waiting for one: a thrown
        // dynamite stick becomes a prop only at the next object scan, after
        // its fuse trail started.
        std::uint64_t createdUs = 0;
        std::uint64_t attachAddress = 0;
        std::int32_t attachBone = -1;
        bool attachResolved = true;
        std::uint32_t attachWaitSinceMs = 0;
        // The track ended when its owner was lost (an actor the recording
        // does not have); the key's later calls add nothing.
        bool ownerLostEnded = false;
        // The key's last call (QPC us) and the last attach record refused as
        // a new tenant's.
        std::uint64_t lastCallUs = 0;
        std::uint64_t tenantAttachUs = 0;
    };
    extern std::vector<LiveRegistered> g_poseV2RegisteredLive;
    extern std::uint64_t g_poseV2FxOwnerLostEnded;
    extern std::uint64_t g_poseV2FxTenantAttaches;
    extern std::uint32_t g_poseV2FxTenantLogs;
    extern std::unordered_map<std::uint64_t, std::uint64_t> g_poseV2FxInstCreatedUs;
    extern std::uint64_t g_poseV2FxAttachTotalSeen;
    extern std::uint32_t g_poseV2FxLateOwnerLogs;
    extern std::uint32_t g_poseV2FxLateLostLogs;
    extern std::uint64_t g_poseV2FxLateResolved;
    extern std::uint64_t g_poseV2FxLateLost;
    extern std::uint64_t g_poseV2FxAttachChanges;
    extern std::uint32_t g_poseV2FxAttachPlayLogs;
    extern std::uint64_t g_poseV2FxAttachPlayApplied;
    extern std::uint64_t g_poseV2FxAttachPlayWaiting;
    // One-shot fx on an entity not recorded yet (skipped_entity): evidence
    // only, whether its entity became a recorded prop or weapon within the
    // same wait (a one-shot is placed at its start, so it is not kept late).
    struct PendingOneShotOwner
    {
        std::uint64_t address = 0;
        std::uint32_t effectHash = 0;
        std::uint32_t sinceMs = 0;
    };
    extern std::vector<PendingOneShotOwner> g_poseV2FxPendingOneShot;
    extern std::uint64_t g_poseV2RegisteredFramesStored;
    extern std::uint64_t g_poseV2RegisteredSkippedOld;
    extern std::uint64_t g_poseV2CameraAmbientSkipped;
    extern std::uint64_t g_poseV2CameraAmbientPlaySkipped;
    extern std::uint64_t g_poseV2RegisteredSplits;
    extern std::uint64_t g_poseV2RegisteredDropped;
    extern std::uint64_t g_poseV2RegisteredTrimmed;
    extern std::size_t g_poseV2RegisteredMaxFrames;
    extern bool g_poseV2RegisteredCapLogged;
    extern std::uint32_t g_poseV2RegisteredCapLogMs;
    extern std::uint64_t g_poseV2RegisteredAttachStale;
    extern std::uint64_t g_poseV2RegisteredHeld;
    // The registered calls without an effect hash (the per-frame get of the
    // script looped effects, script_fx_loop.h) and how each was resolved.
    struct ScriptFxHashlessStats
    {
        std::uint64_t calls = 0;
        std::uint64_t liveKey = 0;
        std::uint64_t identity = 0;
        std::uint64_t mismatch = 0;
        std::uint64_t unresolved = 0;
        std::uint64_t keepAliveSite = 0; // of them, from the script keep-alive site
    };
    extern ScriptFxHashlessStats g_scriptFxHashless;
    constexpr std::size_t kNoAttachChange = static_cast<std::size_t>(-1);
    struct PlayRegistered
    {
        std::size_t track = 0;
        std::uint64_t inst = 0;
        const replay::entity::RegisteredFxFrame* last = nullptr;
        std::uint32_t nextSampleMs = 0;
        std::uint32_t createdMs = 0;
        bool lateLogged = false;
        // The attach change applied to our instance (index into the track's
        // attachTimeline), none while the track's own attach holds.
        std::size_t attachApplied = kNoAttachChange;
        std::size_t attachMissLogged = kNoAttachChange; // evidence: one miss line per change
        // The instance as created (effect rule, hash), so the stop at the
        // track's end touches only our own instance.
        replay::regfx::Identity identity;
        bool identityKnown = false;
        // Our key object (its address + id 0). The engine keeps the owner
        // address in its registry entry and may hand it to callbacks after
        // we stop calling, so keys live for the whole process: never freed,
        // never reused (a deque keeps addresses stable).
        std::uint8_t* ownerKey = nullptr;
        std::uint64_t ownerAddress = 0;       // the puppet it rides (0: none or a world object)
        Entity worldTarget = 0;               // the world object found by model, kept 500 ms
        std::uint64_t worldTargetAddress = 0;
        std::uint64_t worldTargetUntilWallMs = 0;
        std::uint64_t lastRecreateWallMs = 0; // the backoff after a recreation in the same stretch
        std::uint32_t ticksSinceFresh = 0;
        bool attachedOk = false;              // attached by us (the parent is the engine's attach)
        bool inGap = false;                   // inside a stretch without a call
        bool gapJustEnded = false;            // the first call after a stretch (a new instance there is the game's too)
        const replay::entity::FxSettings* appliedSettings = nullptr;
        bool settingsCheck = false;           // the settings read back one tick after the start
        replay::entity::FxSettings checkSettings;
        std::uint64_t pendingTick = 0;        // the last tick it waited as a pending entry
        const char* waitReason = "none";      // why the track has no instance (FX_WORLD_REFUSE_EXPIRED)
    };
    extern std::vector<std::unique_ptr<PlayRegistered>> g_poseV2RegisteredPlay;
    extern std::unordered_map<std::size_t, std::unique_ptr<PlayRegistered>> g_fxPendingEntries;
    extern std::uint64_t g_fxEntryHeldNotReady;
    extern std::uint64_t g_fxHoldNotReady;
    extern std::uint64_t g_fxHoldSkips;
    extern std::uint32_t g_fxRecreateLogsByKind[3];
    extern std::map<std::uint64_t, std::uint32_t> g_fxRecreateLogsByHash;
    // FxWorldOwnerEarly: a map object a track waits to ride is refused to the
    // engine from the first tick the track is in force and found its object,
    // before our instance exists (otherwise the engine makes its own copy in
    // the first drawn frame, while the track still waits). A lease: when our
    // instance is not made within kFxWorldRefuseLeaseMs (a failed creation,
    // an entity never ready) the object leaves the list and the engine's
    // smoke comes back (FX_WORLD_REFUSE_EXPIRED); the lease starts again when
    // the track ends, enters a stretch without a call or gets its instance.
    struct FxEarlyRefuse
    {
        std::uint64_t address = 0;
        std::uint64_t sinceWallMs = 0;
        bool expired = false;
        bool seen = false; // a candidate this tick
    };
    extern std::unordered_map<std::size_t, FxEarlyRefuse> g_fxEarlyRefuse;
    extern std::uint64_t g_fxWorldEarlyTicks;
    extern std::uint64_t g_fxWorldRefuseExpired;
    extern std::uint32_t g_fxWorldRefuseLogs;
    Entity HandleAtAddress(std::uint64_t address);
    Entity FindEntityByModelNear(std::uint32_t model, const float pos[3], float radius);
    extern std::uint64_t g_poseV2RegisteredCreated;
    extern std::uint64_t g_poseV2RegisteredUpdates;
    extern std::uint64_t g_poseV2RegisteredFailed;
    extern std::string g_poseV2RegisteredLastError;
    extern std::uint64_t g_poseV2RegisteredPlaneFailures;
    extern std::string g_poseV2RegisteredPlaneError;
    extern std::uint64_t g_poseV2RegisteredAttachMisses;
    extern std::string g_poseV2RegisteredAttachError;
    extern std::uint32_t g_poseV2RegisteredLastPlayhead;
    extern std::uint64_t g_poseV2RegisteredCrossed;
    // Evidence: does the engine drop an instance we stop calling? Tracks
    // that ended are read back one second later (guarded read of the pooled
    // instance, whatever lives there now) and logged, a few per playback.
    struct EndedRegistered
    {
        std::uint64_t inst = 0;
        std::uint32_t hash = 0;
        std::uint32_t endMs = 0;
        bool attached = false;
        // The instance when its track ended (after our stop), for the one
        // second check (registered_fx_rule.h ClassifyAfterEnd).
        replay::regfx::Identity identity;
        bool identityKnown = false;
    };
    extern std::vector<EndedRegistered> g_poseV2RegisteredEnded;
    extern std::uint32_t g_poseV2RegisteredAfterEndLogged;
    // The ends, the stops and what one second later shows.
    struct RegisteredEndStats
    {
        std::uint64_t ended = 0;
        std::uint64_t stopped = 0;
        std::uint64_t stopFailed = 0;
        std::uint64_t stopNotOurs = 0;  // not playing or another effect at the end: left to the engine
        std::uint64_t scrubKilled = 0;
        std::uint64_t ownerGone = 0;    // stopped because the entity it rides left the replay
        std::uint64_t ownerMissing = 0; // tracks not created: their entity is not in the replay (ticks)
        std::uint64_t ownerMismatch = 0; // not created, recorded on another entity (ticks)
        std::uint64_t notCreatedPaused = 0;
        std::uint64_t afterAlive = 0;
        std::uint64_t afterFinishing = 0;
        std::uint64_t afterReused = 0;
        std::uint64_t afterUnreadable = 0;
        std::string lastStopError;
    };
    extern RegisteredEndStats g_poseV2RegisteredEnd;
    void EndPoseV2RegisteredEntry(const PlayRegistered& entry, const replay::entity::RegisteredFxTrack& track,
        std::uint32_t timeMs, bool immediate, bool watch);
    extern std::uint32_t g_poseV2RegisteredMismatchLogs;
    extern std::unordered_map<std::size_t, std::uint32_t> g_poseV2RegisteredOwnerRetryMs;
    void BeginPoseV2EngineFxStop();

    void PoseV2HiddenPlayerEntities(std::vector<std::uint64_t>& out);
    void StopPoseV2EngineFx(std::uint32_t timeMs);
    void EndPoseV2EngineFxStop(std::ostream& log);
    void PlayPoseV2RegisteredFx(std::uint32_t timeMs);
    void DrainPoseV2FxRegistered(const char* phase);
    void SamplePoseV2FxRegisteredState(const char* phase);
    void LogPoseV2FxRegistered(const char* phase);
    extern std::uint64_t g_poseV2FxAttachesAtStart;
    void LogPoseV2FxStartSample(const char* kind, const replay::fx::FxStart& start);
    extern std::uint64_t g_poseV2FxUnresolvedParent;
    extern std::uint64_t g_poseV2FxInvalidWorld;
    extern std::uint32_t g_poseV2FxUnresolvedLogs;
    void NotePoseV2FxUnresolved(const char* kind, const replay::fx::FxStart& start);
    void LogPoseV2FxCounts(const char* tag, const std::map<std::uint32_t, std::uint32_t>& counts);
    extern std::uint64_t g_poseV2MeleeCursor;
    extern replay::entity::ForwardEventCursor g_poseV2MeleeCursorPlay;
    extern std::uint64_t g_poseV2MeleePlayed;
    extern std::uint64_t g_poseV2MeleeRefused;
    extern std::string g_poseV2MeleeLastStatus;
    extern replay::meleecheck::Tally g_meleeCheck;
    extern std::uint64_t g_poseV2BodyImpactCursor;
    extern std::uint64_t g_poseV2BodyImpactsPlayed;
    extern std::uint64_t g_poseV2BodyImpactsRefused;
    extern std::string g_poseV2BodyImpactLast;
    extern std::uint64_t g_poseV2SoundCursor;
    extern std::uint64_t g_poseV2PlaySoundCursor;
    extern std::uint64_t g_poseV2PlaySoundsCovered;
    extern std::uint32_t g_poseV2PersistentUnattributedLogged;
    extern std::uint32_t g_poseV2SharedHelperLogged;
    extern std::uint32_t g_poseV2ImpactLogged;
    extern std::uint32_t g_poseV2WeaponSoundLogged;

    // Persistent collision sounds of recorded peds: created at startMs,
    // driven by the recorded intensity, stopped at endMs.
    constexpr std::uint32_t kPoseV2CollisionIntensity = 0xEA1AD36D;
    struct PoseV2LiveCollisionSound
    {
        std::uint64_t sound = 0;
        std::uint64_t slot = 0;
        std::size_t index = 0;
    };
    extern std::vector<PoseV2LiveCollisionSound> g_poseV2LiveCollisionSounds;
    extern std::uint64_t g_poseV2SoundStopCursor;
    extern std::uint32_t g_poseV2CollisionLastMs;
    extern std::size_t g_poseV2CollisionNext;
    extern std::uint64_t g_poseV2CollisionPlayed;
    extern std::uint64_t g_poseV2CollisionRefused;
    extern std::string g_poseV2CollisionLast;
    extern std::uint64_t g_poseV2EntitySoundsUnowned;
    extern std::uint64_t g_poseV2EntitySoundsAudioOther;
    extern std::uint64_t g_poseV2PedAudioRecorded;
    extern std::uint64_t g_poseV2PedAudioUnowned;
    void BeginPoseV2EntitySounds();
    extern std::uint64_t g_poseV2ObjectSoundsLate;
    extern std::uint64_t g_poseV2ObjectSoundsExpired;
    extern std::uint64_t g_poseV2ObjectSoundsOneShot;
    void ResetPoseV2ObjectSounds();
    extern std::uint32_t g_poseV2EntitySoundLastMs;
    extern std::uint64_t g_poseV2EntitySoundsPlayed;
    extern std::uint64_t g_poseV2EntitySoundsRefused;
    extern std::string g_poseV2EntitySoundLast;
    extern std::uint32_t g_poseV2EntitySoundPlayLogs;
    extern std::vector<std::uint8_t> g_poseV2EntitySoundWaiting;
    // Per row, what the latest pass did with it (EntitySoundWindowOf). Unlike
    // the waiting states it survives a pause, scrub or jump: reset at the
    // playback start.
    enum : std::uint8_t { kEsndOutcomeNone = 0, kEsndOutcomePlayed = 1, kEsndOutcomeRefused = 2, kEsndOutcomeTooFar = 3,
        kEsndOutcomeByPuppet = 4, kEsndOutcomePassed = 5,
        kEsndOutcomeToGeneric = 6 }; // a packet of the generic player carries the row
    extern std::vector<std::uint8_t> g_poseV2EntitySoundOutcome;
    extern std::unordered_map<std::uint32_t, std::string> g_poseV2EntitySoundLastRefusal;
    extern std::uint64_t g_poseV2EntitySoundsStolen;
    extern std::uint64_t g_poseV2EntitySoundsTooFar;
    extern std::uint64_t g_poseV2EntitySoundsEnded;
    extern std::uint64_t g_poseV2EntitySoundsLate;
    extern std::uint64_t g_poseV2PedAudioLengthStops;
    extern replay::esndtally::Tally g_poseV2EsndTally;
    extern std::vector<std::uint8_t> g_poseV2EsndPassCounted;
    extern bool g_poseV2EsndNewPass;
    // The outcome of each line of speech asked on this pass (owner, event
    // ms), for the ESND rows that are a line's own sound (puppet_audio_rule.h
    // SpeechRowRule). Cleared with the pass.
    struct PoseV2LineOutcome
    {
        std::uint32_t owner = 0;
        std::uint32_t eventMs = 0;
        std::uint32_t pass = 0; // g_poseV2LinePass when it was asked
        bool played = false;
    };
    extern std::vector<PoseV2LineOutcome> g_poseV2LineOutcomes;
    extern std::uint64_t g_poseV2EntitySoundsBySpeech;
    extern std::uint64_t g_poseV2EntitySoundsSpeechFallback;
    extern std::uint64_t g_poseV2EntitySoundsToGeneric;
    extern std::uint64_t g_poseV2SoundsToGeneric;
    extern std::uint64_t g_poseV2CollisionToGeneric;
    extern std::uint64_t g_poseV2AnimToGeneric;
    void NotePoseV2LineOutcome(std::uint32_t owner, std::uint32_t eventMs, bool played);
    extern std::uint64_t g_poseV2PedAudioPlayed;
    extern std::uint64_t g_poseV2PedAudioByPuppet;
    extern std::uint64_t g_poseV2PuppetCreatesSeen;
    void ResetPoseV2PuppetCreates();

    bool PoseV2OwnPersistentRefused(); // below, with the replay's feet
    void PlayPoseV2EntitySounds(std::uint32_t timeMs);
    void StopPoseV2CollisionSounds();
    extern replay::entity::ForwardEventCursor g_poseV2SoundCursorPlay;
    extern std::uint64_t g_poseV2SoundsPlayed;
    extern std::uint64_t g_poseV2SoundsRefused;
    extern std::uint64_t g_poseV2SoundsNeighbor;
    extern std::string g_poseV2SoundLast;
    extern replay::entity::ForwardEventCursor g_poseV2FootstepCursorPlay;
    extern replay::entity::ForwardEventCursor g_poseV2ExplosionCursorPlay;
    extern replay::entity::ForwardEventCursor g_poseV2AnimCursorPlay;
    extern std::uint64_t g_poseV2AnimPlayed;
    extern std::uint64_t g_poseV2AnimRefused;
    extern std::uint64_t g_poseV2AnimNoPuppet;
    extern std::uint64_t g_poseV2AnimRetried;
    extern std::uint64_t g_poseV2AnimConsumed;
    extern std::uint64_t g_poseV2AnimNotConsumed;
    extern std::uint64_t g_poseV2AnimPlayLines;
    extern std::map<std::string, std::uint64_t> g_poseV2AnimRefusals;
    extern std::vector<std::size_t> g_poseV2AnimCarried;
    extern std::vector<std::uint64_t> g_poseV2AnimQueuedAudio;
    extern std::uint64_t g_poseV2FootstepEngineCursor;
    extern std::uint64_t g_poseV2FootstepsPlayed;
    extern std::uint64_t g_poseV2FootstepsRefused;
    extern std::uint64_t g_poseV2FootstepsPuppetEngine;
    extern std::uint64_t g_poseV2FootWetnessRecorded;
    extern std::uint64_t g_poseV2FootWetRecorded;
    extern std::uint64_t g_poseV2FootLayersRecorded;
    extern std::uint64_t g_poseV2FootLayersNonZero;
    extern std::uint64_t g_poseV2FootLodRecorded;
    extern replay::horsesound::Tallies g_poseV2SoundTallies;
    replay::horsesound::SpeechTally& PoseV2SpeechTally(const replay::entity::ReplayId& id, bool animal);
    extern std::string g_poseV2FootstepLastError;
    extern std::map<std::string, std::uint32_t> g_poseV2FootstepRefusals;
    extern std::map<std::string, std::uint32_t> g_poseV2AudioStateLogged;
    extern std::map<std::string, std::uint32_t> g_poseV2FootForced;
    extern std::vector<std::uint32_t> g_poseV2FootLodIds;
    extern bool g_poseV2PuppetAudioLodRecorded;
    extern std::unordered_map<std::uint32_t, std::vector<replay::footmaterial::LodPoint>> g_poseV2FootLodPoints;
    extern std::uint64_t g_poseV2FootLodRecordedWrites;
    extern std::uint64_t g_poseV2FootLodChosen[4];
    extern std::map<std::string, std::uint32_t> g_poseV2FootLodLogged;
    extern std::uint64_t g_poseV2FootLodWrites;
    extern std::unordered_set<std::uint64_t> g_poseV2AudioStateSeen;

    void LogPoseV2FxSnapshot(const char* source, const std::vector<std::uint8_t>& bytes);
    bool PoseV2SpeechLineList(Ped ped, std::uint64_t& list, std::uint16_t& count);
    void TrackPoseV2QueuedLine(Ped ped, replay::entity::ReplayId id, std::uint32_t context,
        const char* phase);
    void FollowPoseV2QueuedLines();

    // Per recorded ped: health (a drop is a hit: bullet or melee) and the
    // speech playing edge.
    struct PoseV2AudioWatch
    {
        Ped ped = 0;
        int health = -1;
        bool speaking = false;
        replay::entity::ReplayId id;
        std::uint64_t address = 0;
        std::uint64_t audio = 0;
    };
    extern std::vector<PoseV2AudioWatch> g_poseV2AudioWatches;
    void WatchPoseV2AudioEvents();

    // The weapon (object +0x448, a validated CWeapon) of every recorded
    // weapon object and prop, remembered 2 s after the object is gone: a
    // thrown weapon's fire event keeps the weapon of the hand object the
    // engine deletes the frame it throws (fire info +0x00).
    struct PoseV2WeaponMemo
    {
        std::uint64_t weapon = 0;
        replay::entity::ReplayId id;
        std::uint32_t lastMs = 0;
    };
    extern std::vector<PoseV2WeaponMemo> g_poseV2WeaponMemo;
    extern std::uint64_t g_poseV2FireRefFromMemo;
    void RememberPoseV2Weapons();
    void FlushPoseV2FxWindows(bool all);

    // Speech events: every accepted request at the speech core (a line was
    // created), from any path, for a recorded ped, with the context it asked
    // for and the line the engine resolved. A line becomes an event when it
    // is heard (its audible start, state 5), timed by that
    // start; a line stopped before it started was never heard and is
    // dropped. A stop before the natural end (reason != 1) is recorded on
    // the event. When the start detour is missing, lines are timed by their
    // request.
    struct PoseV2PendingSpeech
    {
        replay::entity::SpeechEvent event;
        std::uint64_t line = 0;
    };
    // Pending lines (created, waiting for the audible start) and started
    // recorded lines (until they stop) live in one matcher keyed by the line
    // address AND checked by identity: a line created at an address drops
    // what the recorder still holds there (the interaction prompt's options
    // are released without the observed stop), and a start whose voice,
    // context and variation differ from the pending's is not recorded as it.
    using PoseV2SpeechMatcher = replay::speech::rec::LineMatcher<PoseV2PendingSpeech>;
    extern PoseV2SpeechMatcher g_poseV2SpeechMatcher;
    extern std::uint64_t g_poseV2SpeechStartCursor;
    extern std::uint64_t g_poseV2SpeechStopCursor;
    extern std::uint32_t g_poseV2SpeechDroppedUnheard;
    extern std::uint32_t g_poseV2SpeechVoiceInRequest;
    replay::speech::Identity PoseV2SpeechIdentity(const replay::entity::SpeechEvent& line);
    void LogPoseV2SpeechId(const char* phase, const replay::entity::SpeechEvent& line, std::uint64_t linePtr,
        const replay::speech::Identity& id, std::uint64_t voiceObject, std::uint32_t dataType, std::uint32_t path,
        std::uint32_t coreMs, std::int64_t startMs, const std::string& extra);
    void LogPoseV2SpeechDrops(const std::vector<PoseV2SpeechMatcher::Drop>& drops, std::uint32_t atMs);
    void LogPoseV2SpeechStartedReused(const std::vector<PoseV2SpeechMatcher::StartedDrop>& drops, std::uint32_t atMs);
    void DropPoseV2EarlyLine(std::uint64_t line);
    void CollectPoseV2AdoptCandidates(const std::vector<replay::audio::SpeechLineStart>& starts,
        const std::vector<replay::audio::SpeechLineStop>& stops);
    void CollectPoseV2EarlyLines(const std::vector<replay::audio::SpeechCoreCall>& core,
        const std::vector<replay::audio::SpeechLineStart>& starts, const std::vector<replay::audio::SpeechLineStop>& stops);
    void RecordPoseV2SpeechEvents();
    void FlushPoseV2PendingSpeech();
    extern std::uint32_t g_poseV2SurfaceNextMs;
    extern std::map<std::uint32_t, replay::blood::Surface> g_poseV2SurfaceLast;
    extern std::uint64_t g_poseV2SurfacesRecorded;
    extern std::uint64_t g_poseV2SurfacesUnreadable;
    void RecordPoseV2Surfaces();
    extern std::map<std::uint32_t, std::size_t> g_poseV2SurfaceApplied;
    extern bool g_poseV2SurfaceIndexBuilt;
    extern std::uint64_t g_poseV2SurfacesApplied;
    extern std::uint64_t g_poseV2SurfacesFailed;
    extern std::string g_poseV2SurfaceLastError;
    void PlayPoseV2Surfaces(std::uint32_t timeMs);
    extern std::uint64_t g_poseV2AnimCursor;
    extern std::uint64_t g_poseV2AnimLogged;
    extern std::uint64_t g_poseV2AnimBatches;
    extern std::uint64_t g_poseV2AnimRecorded;
    extern std::uint64_t g_poseV2AnimSkippedDuplicate;
    extern std::uint64_t g_poseV2AnimSkippedSpeechHash;
    extern std::uint64_t g_poseV2AnimUnrecordedOwner;
    extern std::uint64_t g_poseV2AnimTailNonZero;
    extern std::uint64_t g_poseV2AnimRecordLines;
    void RecordPoseV2AnimEvents();
    void LogPoseV2AnimAudioRecordTotal();
    void RecordPoseV2Explosions();
    void RecordPoseV2Footsteps();
    extern std::uint64_t g_poseV2FxImpactNoShot;
    extern std::uint64_t g_poseV2FxWorldOwner;
    void TracePoseV2Emitters(const char* phase, std::uint32_t timeMs);
    void RecordPoseV2Fx();

    // Every decal the engine added: the settings as the rage add received
    // them, the attach entity mapped to a recorded id or to a world object by
    // model and position.
    void AddPoseV2DecalEvent(const replay::decal::AddCall& call, std::uint32_t timeMs, bool initial);
    void RecordPoseV2DecalExtras(std::uint64_t now, std::uint32_t nowMs);

    // The decal monitor: the observer runs all the time and this keeps every
    // decal still alive in the world (adds, minus removals, minus the ones the
    // engine dropped, checked a few per tick). A recording starts with them as
    // its initial state, so the playback can clear the area and rebuild it.
    constexpr std::size_t kDecalMonitorMax = 4096;
    extern std::unordered_map<std::uint32_t, replay::decal::AddCall> g_poseV2DecalMonitor;
    extern std::uint64_t g_poseV2MonitorAddCursor;
    extern std::uint64_t g_poseV2MonitorRemoveCursor;
    extern std::vector<std::uint32_t> g_poseV2MonitorCheck;
    extern std::size_t g_poseV2MonitorCheckAt;
    extern bool g_poseV2DecalInitialPending;
    extern std::uint32_t g_poseV2DecalMonitorSession;
    extern bool g_poseV2DecalInitialPlayPending;
    extern std::uint64_t g_poseV2DecalInitial;
    void MonitorPoseV2Decals(bool acceptAdds);
    void UntrackPoseV2RecordedDecal(std::uint32_t id);
    void RecordPoseV2Decals();

    replay::entity::ReplayId RecordedIdAtAddress(std::uint64_t address);

    // Decals in normal play, forward only, added through the game's generic
    // path with the recorded settings on the puppet (or the world object).
    // Decals are state: they play in every transport state. A chain is a
    // recorded decal and its re-adds (a growing blood pool re-adds with the id
    // it was given); the state at a playhead is the latest event of each chain
    // at or before it, unless removed by then. Normal forward play adds events
    // as they come; any jump (scrub either way, seek) reconciles every chain:
    // removes what does not exist yet, puts back the right size, adds what
    // should already be there.
    struct DecalChain
    {
        std::vector<std::size_t> events; // time ascending
        std::uint32_t removedMs = 0xFFFFFFFFu;
        std::uint32_t ours = 0;
        std::size_t applied = static_cast<std::size_t>(-1);
        std::uint32_t appliedAtMs = 0; // the playhead of the last (re)add, for a growing pool
        replay::decal::RegrowRetry regrow; // refused regrows, bounded
    };
    extern std::vector<DecalChain> g_poseV2DecalChains;
    extern std::uint64_t g_poseV2DecalRegrows;
    extern std::uint64_t g_poseV2DecalRegrowRefused;
    extern std::uint64_t g_poseV2DecalRegrowGaveUp;
    extern std::uint64_t g_poseV2DecalRegrowDeferred;
    extern std::uint32_t g_poseV2DecalRegrowLogs;
    extern std::string g_poseV2DecalRegrowLastError;
    extern bool g_poseV2DecalChainsBuilt;
    extern std::uint32_t g_poseV2DecalLastPlayhead;
    extern bool g_poseV2DecalReconcileIncomplete;
    extern bool g_poseV2StateDeferred;
    extern std::uint64_t g_poseV2DeferredTicks;
    extern std::uint64_t g_poseV2DecalReconciles;
    extern std::uint64_t g_poseV2DecalStateRemoved;
    extern std::uint64_t g_poseV2DecalStateRestored;

    // A removal reaches only the decals the core has built. The remove
    // walks the instance lists at mgr+0x3960; an add still waiting
    // in the core's request ring is not there yet and is built after the remove,
    // for good. A scrub adds and removes the short chains of a bleeding body on
    // every tick and backs the ring up, so removed pools would come back and pile
    // up. A removed id is checked again a few times for a short while and
    // removed again while alive, unless it is ours again (id reuse).
    struct PoseV2DecalRemoveRetry
    {
        std::uint32_t id = 0;
        std::uint64_t firstMs = 0;
        std::uint64_t nextMs = 0;
        std::uint32_t tries = 0;
    };
    extern std::vector<PoseV2DecalRemoveRetry> g_poseV2DecalRemoveRetries;
    extern std::uint64_t g_poseV2DecalRemoveRetried;
    extern std::uint64_t g_poseV2EngineDecalCursor;
    extern std::uint64_t g_poseV2EngineDecalsRemoved;
    extern std::uint64_t g_poseV2EngineDecalsOnPeds;
    void PlayPoseV2Decals(std::uint32_t timeMs);
    void PlayPoseV2ProjDecals(std::uint32_t timeMs);
    extern std::uint32_t g_poseV2ObjectAtAddressLogs;
    extern std::uint64_t g_poseV2BloodRef78[3];
    void ZeroFireDescPointers(replay::entity::FireStartEvent& event);
    extern std::uint32_t g_poseV2FireWorldEntities;
    extern std::uint32_t g_poseV2FireWorldFound;
    extern std::uint32_t g_poseV2FireWorldMissing;
    void TakeFireWorldEntity(std::uint64_t address, replay::entity::ReplayId id, std::uint32_t& model, float pos[3]);
    void AuditPoseV2ClipPointers();
    void RecordPoseV2GoreHits();
    void RecordPoseV2Blood();
    void RecordPoseV2UntrackedFires(std::uint32_t timeMs, const Vector3& player);
    void RecordPoseV2InitialFires();
    void SamplePoseV2FireItems(std::uint32_t nowMs);
    void RecordPoseV2HeldState(std::uint32_t nowMs);
    void RecordPoseV2TimeScale();

    // Gore events whose puppet did not exist yet (a ped already mutilated
    // when the recording started, or one that entered with its gore): kept
    // until the puppet appears, then applied (bounded).
    struct PendingGore
    {
        bool hit = false;
        std::size_t index = 0;
        std::uint32_t eventMs = 0;
    };
    extern std::vector<PendingGore> g_poseV2GorePending;
    extern std::map<std::uint32_t, std::uint16_t> g_poseV2GoreApplied;
    extern std::uint32_t g_poseV2GoreLastPlayhead;
    extern std::uint64_t g_poseV2GoreReconciles;
    extern std::uint32_t g_poseV2GoreRetryTicks;
    extern std::uint32_t g_poseV2NotPedLogs;
    void PlayPoseV2GoreHits(std::uint32_t timeMs);
    void PlayPoseV2Gore(std::uint32_t timeMs);
    void PoseV2FireArea();
    void ClearPoseV2Fires(bool instant);
    void SchedulePoseV2FireCheck(const char* tag, const float* center, float radius);
    void CheckPoseV2FireClear();
    void PlayPoseV2Fires(std::uint32_t timeMs);
    void PlayPoseV2Blood(std::uint32_t timeMs);
    void PlayPoseV2DecalRemoves(std::uint32_t timeMs);
    void RecordPoseV2MeleeHits();
    void RecordPoseV2BodyImpacts();
    void RecordPoseV2Sounds();
    void RecordPoseV2PlaySounds();
    void TrackPoseV2CollisionSounds(bool closeAll);

    // Shot events play in forward normal play only, through the recorded
    // weapon's own effects, from the weapon puppet's muzzle when it has one. A
    // shot whose shooter puppet is not there is cancelled.
    constexpr float kPoseV2ShotRangeM = 150.0f;

    // The recorded engine fire event (the shot's or throw's audio), its entity
    // references pointed at the puppets, handed to the same enqueue the weapon
    // code uses; "unmapped:<offsets>" when a reference has no puppet counterpart
    // (nothing is enqueued then). A reference held by a weapon object whose
    // puppet is gone or not made yet (a thrown weapon: the hand object is
    // replaced by the projectile the same frame) is taken from another ready
    // weapon-object puppet of the same weapon, the shooter's first (the
    // projectile).
    constexpr std::uint32_t kPoseV2ShotFireWaitMs = 300;
    struct PoseV2ShotFirePending
    {
        std::size_t shot = 0;
        std::uint32_t untilMs = 0;
    };
    extern std::vector<PoseV2ShotFirePending> g_poseV2ShotFirePending;
    extern std::uint64_t g_poseV2ShotFireViaFallback;
    extern const char* g_poseV2PlayerAudioSource;
    extern std::vector<std::uint64_t> g_poseV2PlayerAudioPeds;
    extern replay::audio::PlayerAudioStats g_poseV2PlayerAudioBase;
    void BeginPoseV2PlayerAudio();
    void LogPoseV2PlayerAudioTotal(const char* phase, std::size_t playerShots);
    extern std::uint64_t g_pwbVerifyAtMs;
    void BeginPoseV2PlayerWeaponBank();
    void EndPoseV2PlayerWeaponBank(bool restore);
    void VerifyPoseV2PlayerWeaponBank();
    extern replay::audio::MountAudioStats g_poseV2MountAudioBase;
    extern bool g_poseV2MountProbeLogged;
    void PoseV2PlayerRiderIds(std::vector<std::uint32_t>& riders);
    void BeginPoseV2PlayerMountAudio();
    void EndPoseV2PlayerMountAudio();
    void ProbePoseV2PlayerMountAudio(std::uint32_t nowMs);
    void LogPoseV2PlayerMountAudioTotal(const char* phase);
    void BeginEntityQuery(bool recording);
    void TickEntityQuery(bool recording, std::uint32_t timeMs);
    void EndEntityQuery(const char* phase);

    // The player state the puppets' ped audio reads (ped_audio_state.h). (a)
    // The foley variant indices the engine draws once per ped (0 and 0 for the
    // local player, random for the rest): recorded per owner (FVAR) and compared
    // with each puppet's (FOLEY_VARIANT evidence; never written: every live ped
    // reads -1/-1 at +0xEC/+0xF0, so the recorded draw never exists live). (b)
    // Evidence only: the PED_VARIABLES block of each recorded owner and puppet
    // (on change) and its difference from the recording at the playhead when the
    // same clip was recorded in this session.
    bool PoseV2IsRecordedPlayer(const replay::playback::Puppet& puppet); // defined with the speech track
    extern bool g_pedSpeedReplay;
    extern std::vector<replay::pedaudiostate::SpeedTarget> g_pedSpeedTargets;
    extern bool g_pedSpeedPublished;
    extern bool g_motionSpeedReplay;
    extern std::vector<replay::pedaudiostate::MotionTarget> g_motionTargets;
    extern bool g_motionPublished;
    extern bool g_footEventReplay;
    extern bool g_footMovementReplay;
    extern replay::entity::ForwardEventCursor g_footEventCursorPlay;
    extern bool g_footEventRefusing;
    void BeginPedAudioState(bool recording);
    void TickPedAudioState(bool recording, std::uint32_t timeMs);
    void EndPedAudioState(const char* phase);
    void TickMotionState(bool recording, std::uint32_t timeMs);
    void TickMovementDetail(bool recording, std::uint32_t timeMs);
    void PublishFootMovement(std::uint32_t timeMs);
    void BeginFootEvents(bool recording);
    void EndFootEvents(const char* phase);
    void RecordFootEvents();
    void DiscardFootEventsWhilePaused();
    void PlayFootEvents(std::uint32_t timeMs);
    void TickPoseV2HorseAudioEvidence(bool recording, std::uint32_t timeMs);
    void BeginPoseV2OwnFeet();
    void UpdatePoseV2OwnFeet();
    extern replay::audio::MovementTermStats g_poseV2MovementTermBase;
    void EndPoseV2OwnFeet(std::ostream& log);
    void PlayPoseV2Shots(std::uint32_t timeMs);

    // Speech events play like shots: forward normal play only, on the
    // speaker's puppet, cancelled when the puppet is not there.

    // Voice preload: changing a ped's voice starts loading it, and a line
    // requested before that finishes dies. The puppet gets the voice of its next
    // line ahead of time.
    constexpr std::uint32_t kPoseV2VoicePreloadMs = 4000;
    // The audible distance of the replay's ambient speech slots (speech_slots.h).
    constexpr float kPoseV2AudibleM = 60.0f;
    extern std::uint64_t g_poseV2SpeechMatches;
    extern std::uint64_t g_poseV2SpeechMismatches;
    extern std::uint64_t g_poseV2SpeechRefused;
    extern std::uint64_t g_poseV2VoicePinned;
    extern std::uint64_t g_poseV2VoiceFailed;
    extern std::uint64_t g_poseV2VoiceAnimal;
    extern std::uint64_t g_poseV2SpeechStopsPlayed;
    extern std::uint64_t g_poseV2SpeechPlayStartCursor;
    extern std::uint32_t g_poseV2SpeechLoadMs;
    void MeasurePoseV2SpeechLoad();

    // Lines the replay started on puppets, followed until they end: their
    // audible start (logged against the recorded one) and the recorded stop.
    struct PoseV2PlayedSpeech
    {
        std::size_t eventIndex = 0;
        Ped ped = 0;
        std::uint64_t line = 0;
        bool startLogged = false;
        bool stopDone = false;
        // An animal's line (speech::RetryAnimalLine), when it was asked (audio
        // clock) and whether it was asked again.
        bool animal = false;
        bool retried = false;
        std::uint64_t requestUs = 0;
        // The playhead of the request, the path and voice status for the
        // speech_silent detail, and whether the line was judged (speech_identity.h
        // JudgeLine).
        std::uint32_t requestPlayMs = 0;
        const char* path = "direct";
        const char* voiceStatus = "";
        bool judged = false;
        bool remade = false;
        bool pinnedAttempt = false; // an animal line asked with the recorded voice
        // Its bank at the request (animal_bank.h).
        replay::animalbank::PinnedBank pinnedBank = replay::animalbank::PinnedBank::Unknown;
        // A line refused a wave slot (the slot reservation found none) is
        // asked again until this audio time, every kPoseV2SlotRetryEveryMs.
        std::uint64_t slotWaitUntilUs = 0;
        std::uint64_t slotNextRetryUs = 0;
        std::uint32_t slotRetries = 0;
        std::uint64_t firstRequestUs = 0;
        bool ownVoiceRetried = false; // closed theater: asked again with the puppet's own voice
        bool ambientRetried = false;  // asked again with the metadata's source
        // Said again after a jump with this start offset (resumed,
        // speech_resume.h), stopped by the replay itself (its recorded stop, a jump,
        // the same-context wait: not a cut), and the audible start on both clocks
        // (the cut diagnosis).
        bool resumed = false;
        std::uint32_t resumeOffsetMs = 0;
        bool stoppedByReplay = false;
        std::uint64_t startUs = 0;
        std::uint32_t startPlayMs = 0;
        // The source the request carried (4 = ambient, 0 = the metadata's) and the
        // one the line got (+0x218).
        std::int32_t askedSource = 0;
        // How the recorded player's scripted line was asked (the retries ask it
        // the same way; speech_script_path.h).
        std::int32_t source = 0;
        // Created on the hidden player with the recorded voice in the request
        // (NamedVoiceOnPlayer); a slot retry asks it so.
        bool namedVoice = false;
        // Said by the hidden player brought to the puppet for it (HoldAtPuppet).
        bool held = false;
        // The recorded player's line on his puppet because the live player is
        // another character: no own voice retry (OwnVoiceAllowed).
        bool noOwnVoice = false;
    };
    extern std::vector<PoseV2PlayedSpeech> g_poseV2PlayedSpeech;
    extern replay::speech::LineCounts g_poseV2SpeechLines;
    extern std::set<std::uint64_t> g_poseV2AmbientFailed;
    struct PoseV2SlotWait
    {
        std::size_t index = 0;
        std::uint32_t requestMs = 0;
        std::uint64_t sinceUs = 0;
        bool resumed = false;
    };
    extern std::vector<PoseV2SlotWait> g_poseV2SlotQueue;
    extern std::uint64_t g_poseV2SlotGateAsks;
    extern std::uint64_t g_poseV2SlotGateWaits;
    extern std::uint64_t g_poseV2SlotGateWaitMaxMs;
    extern std::uint64_t g_poseV2SlotGateWaitAsked;
    extern std::uint64_t g_poseV2SlotGateEvicted;
    extern std::uint64_t g_poseV2SlotGateDropInaudible;
    extern std::uint64_t g_poseV2SlotGateDropLate;
    extern std::uint64_t g_poseV2SlotGateDropPlay;
    extern std::uint64_t g_poseV2SlotIdleReleased;
    extern std::uint64_t g_poseV2SlotIdleFailed;
    extern std::uint64_t g_poseV2SlotCensusUnread;
    extern int g_poseV2SlotMinFree;
    extern std::uint64_t g_poseV2SourceAmbient;
    extern std::uint64_t g_poseV2SourceBank;
    extern std::uint64_t g_poseV2SourceOther;
    extern std::uint64_t g_poseV2AmbientFallbacks;
    extern std::uint64_t g_poseV2AmbientKilledRetries;
    extern std::uint64_t g_poseV2AmbientKilledRetryCreated;
    extern std::uint64_t g_poseV2AmbientRefusedRetries;
    extern std::uint64_t g_poseV2AmbientRefusedRetryCreated;
    extern std::uint64_t g_poseV2State3Cursor;
    extern std::map<std::uint64_t, std::uint32_t> g_poseV2State3Codes;
    extern std::map<std::uint32_t, std::uint64_t> g_poseV2State3ByCode;
    extern std::map<std::string, std::uint64_t> g_poseV2SilentByCause;
    extern std::uint64_t g_poseV2AnimalVocalRetries;
    extern std::uint64_t g_poseV2AnimalVocalCreated;
    extern std::uint64_t g_poseV2AnimalNativePlayed;
    extern std::uint64_t g_poseV2AnimalNativeFallbacks;
    // Puppets whose line asked with the recorded voice died loading (the
    // voice's wave not in the slot's bank, sound +0xEE bit 2); their next lines
    // go by context first, for a while, not for the rest of the playback (the
    // first breath of a horse can die while the banks are still loading at the
    // start). After kContextFirstMs the recorded voice is asked again; each
    // further death doubles the wait (up to kContextFirstMaxMs).
    struct AnimalContextFirst
    {
        ULONGLONG untilMs = 0;
        ULONGLONG waitMs = 0;
    };
    extern std::unordered_map<Ped, AnimalContextFirst> g_poseV2AnimalContextFirst;
    extern std::uint32_t g_poseV2AnimalContextRetries;
    void BeginPoseV2AnimalBanks(std::uint32_t startMs);
    void EndPoseV2AnimalBanks(std::ostream& log);
    extern std::unordered_set<Ped> g_poseV2HumanOwnVoice;
    extern std::uint64_t g_poseV2OwnVoiceRetries;
    extern std::uint64_t g_poseV2OwnVoiceRetryCreated;
    extern std::uint64_t g_poseV2SlotWaits;
    extern std::uint64_t g_poseV2SlotRetries;
    extern std::uint64_t g_poseV2SlotRetryCreated;
    extern replay::speech::request::Tally g_poseV2RequestTally;
    extern std::vector<std::uint32_t> g_poseV2LearnedEndMs;
    extern replay::speech::resume::Watch g_poseV2SpeechWatch;
    extern std::uint64_t g_poseV2SpeechResumes;
    extern std::uint64_t g_poseV2SpeechResumeAsked;
    extern std::uint64_t g_poseV2SpeechResumeCreated;
    extern std::uint64_t g_poseV2SpeechResumeWithOffset;
    extern std::uint64_t g_poseV2SpeechResumeNoPuppet;
    extern std::uint64_t g_poseV2SpeechResumeAnimalSkipped;
    extern std::uint64_t g_poseV2SpeechResumeLate;
    extern std::uint64_t g_poseV2SpeechJumpStops;
    extern std::uint64_t g_poseV2SpeechLeaves;
    extern std::uint64_t g_poseV2SpeechCuts;
    extern std::uint64_t g_poseV2SpeechShortEnds;
    extern std::uint64_t g_poseV2SpeechLearnedEnds;
    extern std::uint64_t g_poseV2OwnSpeechSeenBase;
    extern std::uint64_t g_poseV2OwnSpeechRefusedBase;
    extern std::uint64_t g_poseV2WorldSpeechBase;
    extern std::uint64_t g_poseV2WorldSpeechQueuedBase;
    void BeginPoseV2PuppetSpeech();

    // The speech core refuses a line in state 3 when the speaker's newest active
    // line is another line of the same context (code 18; the line
    // dies with reason 6). Live, the recorded line was accepted, so the
    // speaker's previous line of that context had ended; in playback it still
    // plays for the few tens of ms the replayed start lags. The request waits
    // for that line, as the recorded order says, and at kPoseV2SpeechDeferMaxMs
    // the old line is stopped.
    struct PoseV2DeferredLine
    {
        std::size_t index = 0;
        std::uint32_t requestMs = 0;
        std::uint64_t sinceUs = 0;
        bool stopped = false;
    };
    extern std::vector<PoseV2DeferredLine> g_poseV2DeferredSpeech;
    extern std::uint64_t g_poseV2SpeechDeferred;
    extern std::uint64_t g_poseV2SpeechDeferStops;
    extern std::uint64_t g_poseV2SpeechDeferDropped;
    extern std::uint64_t g_poseV2SpeechDeferOffsetLines;
    extern std::uint32_t g_poseV2SpeechDeferOffsetMaxMs;
    extern std::uint64_t g_poseV2SpeechDeferLateDropped;
    extern std::uint64_t g_poseV2SpeechSyncLines;
    extern std::uint64_t g_poseV2SpeechSyncLate;
    extern std::uint64_t g_poseV2SpeechSyncAhead;
    extern std::int64_t g_poseV2SpeechSyncWorstMs;
    extern std::uint32_t g_poseV2SpeechSyncWorstEventMs;
    extern std::uint64_t g_poseV2SpeechDeferMaxWaitMs;
    extern std::uint64_t g_poseV2AnimalLineCuts;
    extern std::uint64_t g_poseV2AnimalDeferDropped;

    // Jump rule. Leaving normal play (pause, scrub, prepare) or jumping, the
    // replay stops every line it plays (no echo of the old place, no double when
    // the line is said again); back in normal play it says again the line each
    // speaker is inside of, with the time already played as the start offset. A
    // speaker whose puppet is not ready yet (a jump re-creates puppets) is asked
    // when it is, while the line has time left (speech_resume.h).
    struct PoseV2PendingResume
    {
        std::size_t index = 0;
        const char* why = "none";
        std::uint64_t waitSinceUs = 0; // waiting for the old copy since
    };
    extern std::vector<PoseV2PendingResume> g_poseV2PendingResume;
    extern std::vector<replay::speech::resume::StoppedCopy> g_poseV2StoppedCopies;
    extern std::uint64_t g_poseV2ResumeOldStopping;
    extern std::uint64_t g_poseV2ResumeOldGone;
    extern std::uint64_t g_poseV2ResumeOldTimeout;
    extern std::uint64_t g_poseV2ResumeOldWaitMaxMs;
    extern std::uint64_t g_poseV2ResumeOldWaitTotalMs;
    extern std::uint64_t g_poseV2ResumeOldSeen[5];
    extern std::uint32_t g_poseV2ResumeOldLogs;
    void PlayPoseV2Speech(std::uint32_t timeMs);

    // Footsteps play like shots: forward normal play only, on the entity's
    // puppet. Foot-down events the engine raises for a puppet by itself
    // (its own animation) are counted, to see double steps.

    // Evidence for replayed sounds that are accepted but not heard: the
    // sound creations (init-params constructor callers) made inside one
    // replay call, observed only for that call.
    template <typename Call>
    std::string PoseV2SoundsDuring(Call&& call)
    {
        if (!g_poseV2AudioObserverReady)
        {
            call();
            return "observer_off";
        }
        const std::uint64_t from = replay::audio::NowUs();
        replay::audio::SetEnabled(true);
        call();
        replay::audio::SetEnabled(false);
        const std::uint64_t to = replay::audio::NowUs();
        static std::vector<replay::audio::SoundCall> sounds;
        replay::audio::CollectSounds(from, to, sounds);
        std::string text = std::to_string(sounds.size());
        for (std::size_t index = 0; index < sounds.size() && index < 3; ++index)
        {
            char chain[64]{};
            std::snprintf(chain, sizeof(chain), "|0x%X/0x%X/0x%X", sounds[index].callers[0],
                sounds[index].callers[1], sounds[index].callers[2]);
            text += chain;
        }
        return text;
    }
    void PlayPoseV2MeleeHits(std::uint32_t timeMs);
    void PlayPoseV2CollisionSounds(std::uint32_t timeMs);
    extern bool g_shotEnvGroupReplay;
    extern std::uint32_t g_shotEnvLines;
    extern std::uint64_t g_shotEnvGiven;
    extern std::uint64_t g_shotEnvMissing;
    void PlayPoseV2Sounds(std::uint32_t timeMs);
    void PlayPoseV2Fx(std::uint32_t timeMs);
    void PlayPoseV2Explosions(std::uint32_t timeMs);
    void PlayPoseV2AnimAudio(std::uint32_t timeMs);
    void LogPoseV2SoundTallies(std::ostream& log);
    void PlayPoseV2Footsteps(std::uint32_t timeMs);


    void BeginPoseV2FxCheck();
    void EndPoseV2FxCheck(std::ostream& log);
        extern std::vector<std::uint8_t> g_wfxInfoEntries;
        extern bool g_wfxInfoTried;
        extern const char* g_wfxInfoError;

        struct WfxCall
        {
            std::uint8_t kind = 0;
            int shot = -1;
            std::uint32_t intentMs = 0;
            std::uint32_t playheadMs = 0;
            std::uint32_t shooter = replay::weaponfx::kNoId;
            std::uint32_t weapon = replay::weaponfx::kNoId;
            const char* result = "none";
            std::uint32_t createdFx = 0;
            std::uint32_t starts = 0;
            std::uint32_t shellGets = 0;
            int inView = -1;  // the engine's planes, at the weapon puppet
            int camView = -1; // the rendered camera's cone (evidence for the viewport hypothesis)
            int inRange = -1; // muzzle: the rendered camera within the info's range
            float distM = -1.0f;
            const char* infoSource = "none";
            std::uint64_t beganUs = 0, endedUs = 0;
            std::int32_t inCall = -1; // census rows of the shooter or weapon during the call (-1: not drained)
            std::int32_t refused = 0;
            std::int32_t blocked = 0;
            std::uint32_t code = 0;
            std::uint64_t rip = 0;
            bool drained = false;
        };
        struct WfxShotWindow
        {
            std::uint32_t weapon = replay::weaponfx::kNoId;
            std::uint32_t shooter = replay::weaponfx::kNoId;
            std::uint32_t window = 0; // census window of the first call's request (two windows: this and the next)
            std::vector<WfxCall> calls;
            std::int32_t after = 0;
            bool partial = false;
        };
        extern std::vector<WfxShotWindow> g_wfxWindows;
        extern replay::entity::ForwardEventCursor g_wfxCursor;
        extern replay::weaponfx::PlayTotals g_wfxTotals;
        extern std::vector<std::uint8_t> g_wfxShotExtra;
        extern std::vector<std::uint8_t> g_wfxShotCalled;
        extern std::uint32_t g_wfxPlayLogs;
        void WfxFlushWindows(bool partial);
    void TallyPoseV2FxCheckPlay(std::uint32_t timeMs);
    void RebuildFxOwned();
    void CheckFxPending();
}
