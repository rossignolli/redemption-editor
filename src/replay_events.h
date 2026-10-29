#pragma once

// Recorded one-shot and continuous events:
// shots with their muzzle and impact effects, speech lines, footsteps,
// melee hits, body impacts, generic sounds and collision sounds. Recording
// reads the fx and audio observers' rings on the script thread and files
// events on the clip; playback fires them on the puppets in normal play
// only (never paused, preparing or scrubbing), as a replay editor does.

#include "replay_context.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace replay::events
{
    void Attach(Context& context);

    // Recording side, called by the recorder core.
    // Installs the recording observers (inside the mod-load hook batch).
    void WarmUp();
    void BeginRecording();
    void Record();
    // Every tick: the always-on monitors (live decals).
    void Monitor();
    void EndRecording();
    // Where the last EndRecording's time went, in us (POSE_V2_STOP_TIMING ev_* fields).
    struct EndTiming
    {
        std::uint64_t totalsUs = 0;  // gate discard, anim, player, mount and horse audio totals
        std::uint64_t flushUs = 0;   // pending speech, shot fx windows, fx histogram
        std::uint64_t decalsUs = 0;  // last decal pass
        std::uint64_t firesUs = 0;   // last fire pass, fire and blood totals
        std::uint64_t auditUs = 0;   // clip pointer audit, the stop tick's part (the copy)
        std::uint64_t soundsUs = 0;  // audio windows, speech and sound rings, top sound paths
        std::uint64_t meleeUs = 0;   // melee and body impact evidence
        std::uint64_t tracksUs = 0;  // collision and entity sounds closed, track and fx summaries
        std::uint64_t speechUs = 0;  // the ring's speech lines (handed to a worker)
        std::uint64_t tailUs = 0;    // fx observer stop, gameplay snapshot, sound capture end
    };
    EndTiming LastEndTiming();
    std::uint32_t ShotImpacts();
    // Research: the effect hash the bench watches (late snapshots).
    void SetWatchEffect(std::uint32_t effectHash);
    // Bench control: playback of the fx, registered fx and decal tracks on
    // or off (a control run shows what the world alone still has).
    void SetFxReplay(bool enabled);
    // Research: the audio-path and fx-trigger windows logged around each
    // shot, hit and speech line (off by default).
    void SetResearchWindows(bool enabled);
    // Evidence: how many decals of the last recording are still alive in
    // the engine (by their recorded ids).
    std::uint32_t RecordedDecalsAlive(std::uint32_t& checked);
    // Removes every decal of the last recording still alive, by its engine
    // id (REMOVE_DECALS_IN_RANGE leaves them).
    std::uint32_t RemoveRecordedDecals();

    // Playback side, called by the playback core.
    void BeginPlayback(std::uint32_t firstCreateMs);
    void Play(std::uint32_t timeMs);
    void EndPlayback();
    // A script restart with a playback open: fx,
    // decals, fires and the detours' lists forgotten without a native on
    // the old world's ids; the sounds in the engine's weak slots stopped by
    // the guarded path (the count of what was forgotten).
    std::uint32_t ForgetPlaybackAfterRestart();
    // Every script restart (save load) empties the
    // always-on decal monitor (the old world's decals, info pointers and
    // engine ids); POSE_V2_DECAL_MONITOR_CLEARED. Returns how many it held.
    std::uint32_t ForgetDecalMonitorAfterRestart();

    // What the entity sound track (ESND) did with its rows whose start is in a playhead
    // window, for the QA mark: per row the latest pass's outcome (played,
    // refused by the engine, too far, left to the puppet's own, its chance
    // gone without a start, not reached), totals and per owner.
    struct EntitySoundWindowOwner
    {
        std::uint32_t owner = 0; // packed replay id
        std::uint32_t rows = 0, played = 0, refused = 0, tooFar = 0, byPuppet = 0, passed = 0, untried = 0;
        std::uint32_t toGeneric = 0; // rows a generic packet carries
        std::string lastStatus; // the last refusal's status ("" none)
    };
    struct EntitySoundWindow
    {
        std::uint32_t rows = 0, played = 0, refused = 0, tooFar = 0, byPuppet = 0, passed = 0, untried = 0;
        std::uint32_t toGeneric = 0; // rows a generic packet carries
        std::vector<EntitySoundWindowOwner> owners; // most rows first
    };
    EntitySoundWindow EntitySoundWindowOf(std::uint32_t fromMs, std::uint32_t toMs);

    // Cheap scrub (a replay editor's scrub is play at cursor speed, not a
    // jump per tick). While deferred, the persistent state (fires, blood,
    // gore, decals, the trail map) is not reconciled per tick; the first
    // tick after it reconciles once at the playhead. The one-shot events
    // never play outside normal play anyway.
    void SetStateDeferred(bool deferred);
    bool StateDeferred();
    // A reconcile still going on (decal budget, gore retries): the seek
    // settle waits for it.
    bool StatePending();
    // The time budget per tick of the state rebuild queue (the
    // blood records after a jump): normal play, settle, frozen picture.
    void SetStateBudget(bool settling, bool frozen);
    // EngineFeetInReplay in the ini. Off (the default): while a
    // clip plays, the puppets' and the hidden player's own foot-down audio
    // and the engine's foot puffs are kept out (the recorded ones play).
    void SetEngineFeetInReplay(bool on);
    // FxOwnerRule in the ini. On (the default): while a clip plays, a one-shot
    // effect the engine starts on its own on a puppet or the hidden player is
    // killed at its start (the recorded one plays); blood, explosion, fire and
    // the weapon sites are exempt. 0: not killed.
    void SetFxOwnerRule(bool on);
    // FxRegisteredSplit in the ini. On (the default):
    // every creation of a registered effect key while recording starts its
    // own track (the hoof dust bursts); 0: one track per live key.
    void SetFxRegisteredSplit(bool on);
    // The replay buffer's trim: the registered fx tracks it emptied (ended
    // before keepFrom) are removed, the live keys' track indices follow
    // (entity::DropEmptiedRegisteredFx). Returns the tracks removed.
    std::size_t DropTrimmedRegisteredFx(std::uint32_t keepFrom);
    // The keys of the particle fixes (README, "Particles"). Each one at 0
    // turns its part off.
    struct FxParticlesConfig
    {
        bool motionProbe = true;          // FxMotionProbe (read only, capped lines)
        bool puppetNoWarp = true;         // FxPuppetNoWarp
        bool recordSettings = true;       // FxRecordSettings (FXSS and RGSS settings)
        bool settingsReplay = true;       // FxSettingsReplay
        bool registeredParentOnce = true; // FxRegisteredParentOnce
        bool callGaps = true;             // FxRegisteredCallGaps (recording and playback)
        bool ownerReady = true;           // FxRegisteredOwnerReady
        std::uint32_t recreateBackoffMs = 250; // FxRegisteredRecreateBackoffMs
        bool puppetVelocity = true;       // FxPuppetVelocity (detour 0x1405BA4)
        bool worldOwnerRule = true;       // FxWorldOwnerRule
        bool ownerListImmediate = true;   // FxOwnerListImmediate
        bool weaponFxEvents = true;       // WeaponFxEvents (recording)
        bool applyRecordedFlags = true;   // FxApplyRecordedFlags
        bool shotLegacyDedupe = true;     // ShotFxLegacyDedupe (older clips)
        bool pauseFreeze = true;          // FxPauseFreeze (detour 0x145C17C)
        bool checkMotion = true;          // FxCheckMotion (detection)
        bool ownerReadyHold = true;       // FxRegisteredOwnerReadyHold: the ready check in every mode, no recreation paused
        bool worldOwnerEarly = true;      // FxWorldOwnerEarly: the map object refused before our instance, 1 s lease
        bool relocateClearStill = true;   // FxRelocateClearStill: the relocate mark cleared on an owner that did not move
        bool weaponFxIntent = true;       // WeaponFxIntent: the shot's effect requests recorded (detours 0x14309A8, 0x14308F0) and played back through the same wrappers
    };
    void SetFxParticlesConfig(const FxParticlesConfig& config);
    // The playback's placement of a puppet
    // (replay_playback.cpp, around PlaceClone): before and after, with the
    // step of this placement; then once at the end of the tick, after every
    // placement and the events, the relocate marks of our effects on the
    // placed puppets are cleared (FxPuppetNoWarp) and sampled (fx check).
    void FxBeforePlace(std::uint64_t puppetAddress);
    void FxAfterPlace(std::uint64_t puppetAddress, float stepM);
    void FxEndOfPlaybackTick(bool normalPlay);
    // The clip's WFXI requests in normal forward play, through the weapon
    // system's own wrappers on the puppets (WeaponFxIntent). Called after
    // Play, outside its audio scope: a sound the call makes is the puppet's
    // own to the sound rules, and it is measured.
    void PlayWeaponFxIntents(std::uint32_t timeMs);
    // FxWorldSiteRule (1): with FxOwnerRule, the engine's one-shots from the
    // sites of the clip's world one-shots are killed at their start while it plays (the fx track plays them).
    void SetFxWorldSiteRule(bool on);
    // SpeechPlayerOnPlayer (1): the recorded player's lines are said by the hidden player.
    void SetSpeechPlayerOnPlayer(bool on);
    // The trail follow (script.cpp UpdatePlayerFollow) reports every tick whether
    // it moved the hidden player (false: off, skipped for a mission or a vehicle,
    // no focus). While a line of the recorded player that names its voice (his
    // pain voices) is held on the hidden player at his puppet
    // (speech_player_route.h HoldAtPuppet), PlayerSpeechHoldAt gives the puppet's
    // position, where the follow keeps him instead of the trail focus.
    void NotePlayerFollow(bool moving);
    bool PlayerSpeechHoldAt(float& x, float& y, float& z);
    // FireUntrackedInitial (0): 1: the
    // engine's fires the start detour never saw enter the clip as initial fires.
    void SetFireUntrackedInitial(bool on);
    void SetFireAdoptLive(bool on); // FireAdoptLive (default 0): keep a live initial fire instead of the native clear and the relight
    // PedAudioLoopFollowsPuppet in the ini. On (the default): a replayed ped
    // audio loop whose recorded params carry a position follows its puppet,
    // as the engine moves it every frame; off: it stays where it was created.
    void SetPedAudioLoopFollowsPuppet(bool on);
    // PedAudioKeepRecordedLength
    // in the ini. On (the default): a replayed short ped / train car audio
    // sound stops after its recorded length (the horse's braking slide is a
    // new sound every frame, each stopped by the game a frame later); off: it
    // plays its whole sample.
    void SetPedAudioKeepRecordedLength(bool on);
    bool PedAudioKeepRecordedLength();
    // AnimalVocalRetry in the ini. On (the default): an animal line
    // the engine killed while loading is asked again through the animal's
    // own vocalization; off: left silent.
    void SetAnimalVocalRetry(bool on);
    bool AnimalVocalRetry();
    // SpeechQueueAnyContext in the ini. On: a speaker's due line waits for
    // any line of that speaker still active (one speech slot per speech
    // entity), not only one of the same context.
    void SetSpeechQueueAnyContext(bool on);
    // PlayerWeaponBank=1 (player_weapon_bank_core.h):
    // the hidden live player equips the recorded player's next shot weapon so
    // the engine has its bank in the player's weapon wave slots.
    void SetPlayerWeaponBank(bool on);
    // PuppetFoleyVariants (ped_audio_state.h; the
    // recorded foley variant indices written into the puppets), PedAudioStateLog
    // (FOLEY_VARIANT and PED_VARS lines) and PedVariablesReadProbe (the
    // read-only detour on the sound variable lookup, installed at the first
    // recording or playback when on).
    void SetPedAudioState(bool variants, bool log, bool readProbe);
    // ped_speed_core.h: PedSpeedRecord (1) keeps each recorded ped's and horse's PED_VARIABLES
    // speed 4 times a second (section PSPD); PedSpeedReplay (1) writes it into
    // the puppet's block after the engine's update (detour on 0x31A3ABC), from
    // the section or the puppet's recorded motion; PedSpeedReplayLog (1) the
    // PED_SPEED_REPLAY lines (one per puppet every 5 s of clip time).
    void SetPedSpeed(bool record, bool replayOn, bool log);
    // foot_event_core.h: FootEventRecord (1) keeps the entries the engine queued in
    // each recorded ped's and horse's second foot queue (the secondary steps,
    // section FSEV); FootEventReplay (1) queues them again on the puppet at
    // the same clip time and, for a clip with FSEV, refuses the puppet's own;
    // FootEventLog (1) the FOOT_EVENT_WINDOW lines (per owner and 5 s).
    void SetFootEvents(bool record, bool replayOn, bool log);
    // MotionSpeedReplay (1): the movement component's own speed (C +0x5DC)
    // on the puppets (detour on 0x8AABCC, the target of PedSpeedReplay);
    // MotionStateLog (1): the MOTION_STATE (per horse and second) and
    // MOTION_SPEED_REPLAY (per puppet and 5 s) lines.
    void SetMotionSpeed(bool replayOn, bool stateLog);
    // ShotEnvGroupReplay (1): a carried weapon's sound the live game made
    // with an environment group gets its holder's (SHOT_ENV lines).
    void SetShotEnvGroup(bool on);
    // foot_movement_core.h: FootMovementRecord (1) keeps the
    // movement volume result of each recorded step (section FMOV);
    // FootMovementReplay (1) gives it back to the puppet's step at the same
    // clip time (no MovementTermCorrection on top; a clip without FMOV is
    // not changed). FOOT_MOVEMENT_TOTAL at each phase's end.
    void SetFootMovement(bool record, bool replayOn);
    // HumanOwnVoiceFallback (default 1). A puppet whose
    // ambient voice line died with the wave missing in its fixed slot asks
    // with its own voice (context and variation kept).
    void SetHumanOwnVoiceFallback(bool on);
    bool SpeechQueueAnyContext();
    // RegisteredFxOwnerCheck
    // in the ini. On (the default): a registered effect's track takes no owner
    // from a new tenant of its instance address after the key stopped calling,
    // and an attached track whose recorded parent is far from the puppet is not
    // created on it.
    void SetRegisteredFxOwnerCheck(bool on);
    bool RegisteredFxOwnerCheck();
    struct StateCounters
    {
        std::uint64_t decalReconciles = 0;
        std::uint64_t bloodReconciles = 0; // per ped
        std::uint64_t goreReconciles = 0;  // per ped
        std::uint64_t fireReconciles = 0;
        std::uint64_t deferredTicks = 0;
    };
    StateCounters Counters();
}
