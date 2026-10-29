#pragma once

// Read-only observer of the engine particle trigger.
//
// Every triggered particle effect, from game code (weapon vfx and the rest)
// and from scripts, goes through one engine function that takes the effect
// hash and the asset hash and returns the effect instance. A detour on it
// records
// each call into a ring: time, hashes, whether an instance came back and
// the return addresses (as RVAs) of its callers. Nothing is changed: the
// original runs with the same arguments and its result is returned as is.
//
// The detour never calls natives. Entries are read on the script thread.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "entity_events.h" // entity::FxSettings

namespace replay::fx
{
    constexpr std::uint32_t kCallerDepth = 4;

    struct TriggerCall
    {
        std::uint64_t timeUs = 0; // QPC microseconds
        std::uint32_t effectHash = 0;
        std::uint32_t assetHash = 0;
        bool instance = false;
        std::uint32_t callers[kCallerDepth] = {}; // RVAs, 0 outside the module
        std::uint32_t threadId = 0;
        std::uint64_t inst = 0; // the instance the trigger returned
    };

    // Every particle instance start: the start detour reads the
    // instance as the engine finished setting it up, matched with the
    // trigger call that returned the instance (effect and asset hashes,
    // caller) and with the attach call on it (entity and bone). Instance
    // layout, offline evidence in the build 1491.50 dump (script start
    // 0x2C50C88, set matrix 0x29E4274, world parent 0x29E2754, attach core
    // 0x2C2FB60): +0x210 parent matrix (identity for a world effect), +0x250
    // base matrix (4 x float4; the offset on the bone when attached, the
    // world matrix otherwise; +0x280 is its position row), +0x394 zoom,
    // +0x580 flags, +0x5D0 mode (1 attached, 2 world).
    struct FxStart
    {
        std::uint64_t timeUs = 0;
        std::uint64_t inst = 0;
        std::uint32_t effectHash = 0; // 0 when no trigger call matched
        std::uint32_t assetHash = 0;
        std::uint32_t callerSite = 0; // callers[1] of the trigger call
        std::uint64_t entity = 0;     // attach entity (mode 1)
        std::int32_t bone = -1;
        std::uint8_t mode = 0;
        std::uint32_t callers[kCallerDepth] = {}; // RVAs of the start callers (evidence)
        float zoom = 1.0f;
        std::uint32_t flags = 0;
        float matrix[16] = {};
        float parent[16] = {};       // +0x210 parent matrix
        std::uint8_t evolutions[16] = {}; // +0x3E0: evolution values (0..255) by rule index
        std::uint32_t threadId = 0;
        // Started inside the replay's own playback tick (ReplayScope) or by
        // its registered core call, and killed right after its start by the
        // one-shot owner rule (SetOneShotOwnerRule).
        bool byReplay = false;
        bool killed = false;
        // The fields the site set before the start.
        entity::FxSettings settings;
    };
    constexpr std::size_t kInstParentMatrix = 0x210;
    constexpr std::size_t kInstMatrix = 0x250;
    constexpr std::size_t kInstMode = 0x5D0;
    // The evolution setter (0x29E33CC, offline) stores value * 255 at
    // inst + 0x3E0 + index, the index from the rule's evolution table.
    constexpr std::size_t kInstEvolutions = 0x3E0;
    constexpr std::size_t kEvolutionBytes = 16;

    // Build 1491.50 weapon vfx call sites of the trigger (return address in
    // the caller of the one-argument wrapper), from a live log: every shot
    // triggered one effect from each, the muzzle one constant per weapon and
    // the impact one varying with the surface.
    constexpr std::uint32_t kMuzzleFlashSite = 0x1452671;
    constexpr std::uint32_t kBulletImpactSite = 0x144D33A;
    // Offline in the same dump:
    // - the ped foot vfx one-shot site (0x1453178, whose only caller is the
    //   per-ped foot vfx 0x14357D0 at 0x1435D15): dust, mud and splash puffs
    //   of feet and hooves (ped_foot_dust 0x83E666D7 among them), placed in
    //   the world. The recording keeps every one (FXEV, caller 0x145320F);
    //   during playback the puppets and the hidden player raise their own on
    //   top (about twice the recorded count), puffs at feet the replay does
    //   not show.
    // - the camera ambient registered effects (0x1469DC1, the loop of
    //   ambient definitions placing each one `distance` metres ahead of the
    //   rendered camera, [r12+0x30] + forward * [entry+4]): recorded as a
    //   world track they would play along the recorded camera's path; the
    //   engine makes its own ahead of the replay camera.
    constexpr std::uint32_t kFootVfxSite = 0x145320F;
    constexpr std::uint32_t kCameraAmbientFxSite = 0x1469DC1;
    // While on, the engine's own triggers from kFootVfxSite get no instance
    // (the site skips a null one). The replay's recorded puffs are played by
    // our own code (another caller) and are not affected. The refused calls
    // stay in the trigger ring with instance=false (the play histogram shows
    // calls/instances). Script thread sets it; any thread reads it.
    void SetEngineFootVfxSuppressed(bool suppressed);
    std::uint64_t SuppressedFootVfx();
    // The weapon sites stay out of the one-shot
    // owner rule only for a clip whose shot track makes the weapon effects
    // (recorded before WeaponFxEvents); a clip with them as events has the
    // engine's own on the puppets killed like any other.
    void SetWeaponSitesExempt(bool exempt);

    // Signature-checked install; false with a reason when the build differs.
    bool Install(std::string& error);

    // Plays a triggered effect by hash through the engine function the
    // particle natives end in (after hashing the name): get the instance,
    // attach it to `entity` at `bone` (or place it in the world when entity
    // is null), set its matrix (offset when attached), zoom and start it.
    // matrix: 4 x float4 = X axis, Y axis, Z axis, position.
    bool EnsurePlayer(std::string& error);
    bool PlayEffect(
        std::uint32_t effectHash,
        void* entity,
        int bone,
        const float matrix[16],
        float scale,
        std::string& error);
    // Muzzle flash as the game's weapon vfx plays it (call site
    // kMuzzleFlashSite, default branch): triggered instance, attached to
    // `entity` at `bone`, then the attachment flags that branch sets (bit 14
    // cleared, bits 22-23 set: the emitted particles do not stay glued to
    // the bone), a zero offset, the zoom, and start.
    // Bits of the instance flags the weapon muzzle code sets after attaching
    // (0x10 from the entity's model, bits 13/14/28, 0xC00000).
    constexpr std::uint32_t kMuzzleFlagMask = 0x10u | (1u << 13) | (1u << 14) | (1u << 28) | 0xC00000u;
    constexpr std::uint32_t kMuzzleDefaultFlags = 0xC00000u;

    // `flags`: the recorded kMuzzleFlagMask bits (kMuzzleDefaultFlags when
    // none was recorded).
    // Plays a recorded instance start the way the engine set it up: the
    // triggered instance, attached to `entity` at `bone` (node, mode 1) or
    // placed by `parent` (the world matrix in the parent, as the engine's
    // own world effects), the offset matrix, the recorded evolution bytes,
    // the zoom, then start. Every engine function is signature-checked.
    bool PlayEffectAs(
        std::uint32_t effectHash,
        void* entity,
        int bone,
        const float offset[16],
        const float parent[16],
        const std::uint8_t evolutions[kEvolutionBytes],
        float zoom,
        std::string& error);
    // The same with the recorded state of the
    // instance: the settings written before the start (ApplyInstanceSettings),
    // the recorded attach bits (kMuzzleFlagMask) on an attached instance, and
    // the instance made (for the one tick later checks).
    struct PlayOptions
    {
        const entity::FxSettings* settings = nullptr;
        bool applyFlags = false;
        std::uint32_t flags = 0;
        std::uint64_t* instOut = nullptr;
    };
    bool PlayEffectAsEx(
        std::uint32_t effectHash,
        void* entity,
        int bone,
        const float offset[16],
        const float parent[16],
        const std::uint8_t evolutions[kEvolutionBytes],
        float zoom,
        const PlayOptions& options,
        std::string& error);

    bool PlayAttachedWeaponEffect(
        std::uint32_t effectHash,
        void* entity,
        int bone,
        float scale,
        std::uint32_t flags,
        std::string& error);

    // Flags of every gameplay muzzle instance at its start (needs the start
    // observer), for the shots recorded in that window.
    struct MuzzleStart
    {
        std::uint64_t timeUs = 0;
        std::uint32_t flags = 0;
    };
    void CollectMuzzleStarts(std::uint64_t fromUs, std::uint64_t toUs, std::vector<MuzzleStart>& out);

    // Research: the effect instance as it is at start, for the
    // first muzzle flash of the recording (game code) and the first one the
    // replay starts, so the two can be compared field by field.
    constexpr std::size_t kSnapshotBytes = 0x960; // header .. the three plane slots
    // Research: watch an effect hash; the next gameplay instance of it is
    // snapshotted at its start and the next replay one right before its
    // start (both flags reset). 0 goes back to the muzzle flash site.
    void SetWatchEffect(std::uint32_t effectHash);
    // Research: copies the instance (ours, right before its start) as the
    // replay snapshot when none was taken yet.
    void SnapshotReplayInstance(std::uint64_t inst);
    // Research: a later snapshot of any instance (the watched one about a
    // second after its start, on either side), logged by the caller.
    bool SnapshotInstance(std::uint64_t inst, std::vector<std::uint8_t>& out);
    bool TakeGameplaySnapshot(std::vector<std::uint8_t>& out);
    bool TakeReplaySnapshot(std::vector<std::uint8_t>& out);

    void SetEnabled(bool enabled);
    std::uint64_t NowUs();

    // The owner model for one-shot effects, as a replay editor usually
    // handles them. While the
    // replay plays, an instance the engine starts on its own (outside the
    // replay's tick on the script thread, the ReplayScope below) attached to
    // a puppet or the hidden player (the SetSuppressedOwners list) is the
    // puppet's own effect: the recorded one is played by the fx track. The
    // start detour lets the engine start it (so the instance is in the
    // engine's active list) and kills it right away through the engine's
    // own stop (0x29E7C3C with 1: state 4, recycled by the engine, the path
    // the registry and the replay's scrub already use) before its first
    // update emits anything. The weapon sites (muzzle, bullet impact) and
    // the hashes in `exempt` (the families with their own tracks: blood,
    // explosion, fire) are never touched. The stop prologue is checked
    // once; a mismatch leaves the rule off with an error.
    bool SetOneShotOwnerRule(bool on, const std::vector<std::uint32_t>& exempt, std::string& error);
    std::uint64_t OneShotOwnerKilled();
    // The world side of the owner rule. The engine
    // sites whose world one-shots the clip carries (an ambient snow gust, a
    // map emitter's puff: recorded as world events and played by the fx
    // track) are the replay's while it plays: an engine start from one of
    // them, outside the replay's tick, is killed the same way. Empty: off.
    void SetWorldSiteRule(const std::vector<std::uint32_t>& sites);
    // Marks the current thread as the replay's playback tick (RAII); the
    // starts made inside are the replay's (FxStart::byReplay).
    class ReplayScope
    {
    public:
        ReplayScope();
        ~ReplayScope();
        ReplayScope(const ReplayScope&) = delete;
        ReplayScope& operator=(const ReplayScope&) = delete;
    };
    // Calls with fromUs <= timeUs <= toUs still in the ring.
    void Collect(std::uint64_t fromUs, std::uint64_t toUs, std::vector<TriggerCall>& out);
    std::uint64_t TotalCalls();
    // Instance starts recorded while enabled, from `cursor` (a start index)
    // on; cursor moves to the end. Starts older than the ring are lost.
    std::uint64_t TotalStarts();
    std::uint64_t TotalAttaches();
    // The last attach call seen on an instance (entity address and bone),
    // false when none is in the ring.
    // The last attach call recorded for this instance address, at or after
    // sinceUs (the instance's own creation: the engine pools instances and an
    // older record belongs to a previous tenant of the address). whenUs gets
    // the record's time.
    bool LastAttachOf(std::uint64_t inst, std::uint64_t& entity, std::int32_t& bone, std::uint64_t sinceUs = 0,
        std::uint64_t* whenUs = nullptr);
    // Attaches an instance of ours to an entity at a bone through the
    // engine's attach (the wrapper the vfx code uses), SEH-guarded.
    bool AttachInstance(std::uint64_t inst, void* entity, int bone, std::string& error);

    // Registered (continuous) effects: every call of the
    // engine's get-or-create by key (owner address + id, effect hash; core
    // 0x2C49B08, 118 call sites in build 1491.50): the instance it returned
    // and whether it was created by this call. Read-only detour.
    struct RegisteredCall
    {
        std::uint64_t timeUs = 0;
        std::uint64_t owner = 0;
        std::int32_t id = 0;
        std::uint64_t arg3 = 0;
        std::uint32_t effectHash = 0;
        std::uint32_t assetHash = 0;
        std::uint64_t inst = 0;
        std::uint8_t created = 0;
        std::uint8_t flag = 0;   // r9b of the core call
        std::uint8_t arg8 = 0;
        std::uint8_t arg9 = 0;
        std::uint32_t caller = 0; // RVA of the call site
        std::uint32_t threadId = 0;
        // Made by the replay's own RegisteredGetOrCreate (thread flag; it
        // calls the trampoline, so it never reaches the ring), and refused
        // because its owner is a puppet (SetSuppressedOwners).
        bool byReplay = false;
        bool suppressed = false;
    };
    // During playback the engine keeps making registered effects for the
    // puppets themselves (the bleeding 0x45FAF2C0 several times over the
    // recorded count), duplicating the recorded tracks the replay plays with
    // its own keys. An engine call of the registered core whose owner is one
    // of these addresses (the puppet clones) is refused: the original is not
    // called and it returns null with created 0, the result the core itself
    // gives when the effect rule is not accepted, so the engine's sites
    // handle it; a key not called is dropped by the engine, as a replay
    // track past its end. Calls made by the replay (thread flag) always
    // pass. An empty list turns it off.
    void SetSuppressedOwners(const std::vector<std::uint64_t>& owners);
    std::uint64_t TotalSuppressed();
    // State of an instance (any instance, guarded reads): the fields the
    // start observer records, for a registered instance sampled per tick.
    struct InstanceState
    {
        float matrix[16] = {};
        float parent[16] = {};
        std::uint8_t evolutions[16] = {};
        float zoom = 1.0f;
        std::uint32_t flags = 0;
        std::uint8_t mode = 0;
        std::uint32_t state = 0; // +0x344
        float timer3f0 = 0.0f;   // +0x3F0, a per-instance random
        std::uint32_t loops = 0;    // +0x320 loops completed
        float loopTime = 0.0f;      // +0x32C normalized time in the loop
        std::uint32_t flags2 = 0;   // +0x584
        float start310[4] = {};     // +0x310: a tracer's start point, written only at creation
        float farClip3C4 = -1.0f;   // +0x3C4: far clip override (a triggered tracer's length)
        std::uint8_t byte341 = 0;   // +0x341
        // Collision planes of the current quality level (offline: 0x29E41E0
        // appends them at inst + lod * 0x310 + 0x630, count at + 0x930, lod
        // from the byte at [[0x4F892F0] + 1]); a ground fire spawns its flames
        // on them. Up to 16 planes of 3 float4 rows.
        std::uint32_t planeCount = 0;
        float planes[16 * 12] = {};
        float aux350[4] = {}; // +0x350, set by 0x2C4F508 from the entity
        std::uint8_t quality = 0xFF;      // evidence
        std::uint32_t slotCounts[3] = {}; // evidence: counts at the three quality slots
        // The settings (entity::FxSettings) and +0x360,
        // the inherited displacement per frame (0x2C523CE: the entity's
        // velocity, vfunc +0xB0, times the step).
        entity::FxSettings settings;
        float veladd360[4] = {};
    };
    constexpr std::uint32_t kMaxFxPlanes = 16;
    bool ReadInstanceState(std::uint64_t inst, InstanceState& out);

    // What says
    // an instance is still the one we made: its effect rule (the qword at
    // +0, whose +0x200 is the effect hash the registered core compares,
    // 0x2C49BAE), its state (+0x344: 2 playing, 3 stopping, 4 killed, 5
    // finished) and its world position (offset +0x250 composed with the
    // parent +0x210).
    struct InstanceIdentity
    {
        std::uint64_t rule = 0;
        std::uint32_t hash = 0; // rule +0x200
        std::uint32_t state = 0;
        float position[3] = {};
        float offset[3] = {}; // +0x280 (the offset on the bone when attached)
    };
    bool ReadInstanceIdentity(std::uint64_t inst, InstanceIdentity& out);

    // The settings of an instance (guarded reads)
    // and written back on an instance of ours before its start (plain
    // writes of the fields of kApplied that are in the mask; +0x350 through
    // the engine's setter 0x2C4F508 when it passed its check).
    bool ReadInstanceSettings(std::uint64_t inst, entity::FxSettings& out);
    bool ApplyInstanceSettings(std::uint64_t inst, const entity::FxSettings& settings, std::string& error);

    // The motion fields of an instance
    // (guarded): +0x584, +0x3A8, +0x360 and the state +0x344.
    struct MotionFields
    {
        std::uint32_t flags2 = 0;
        float tele3A8 = 0.0f;
        float veladd[4] = {};
        std::uint32_t state = 0;
        std::uint32_t hash = 0; // the effect it holds now (rule +0x200), 0 unread
    };
    bool ReadMotionFields(std::uint64_t inst, MotionFields& out);
    // Clears the relocate bit (0x2 of +0x584) of
    // instances of ours under the particle manager's lock, the way the
    // parent setter 0x29E2754 takes it (0x29E2773..0x29E27E3: manager
    // [0x3EBD698], lock object +0x3E280, owner test 0x29DB42C, lock
    // 0x25A1658, unlock 0x25A5694; every prologue and the global's load
    // checked once). An `and` per instance, guarded. False with an error and
    // nothing written when a check failed. Each instance comes with the
    // effect hash it should hold; under the lock one that holds another
    // effect or is not playing/stopping is left alone and counted in `stale`
    // (the engine recycled it).
    bool ClearRelocateMarks(const std::vector<std::pair<std::uint64_t, std::uint32_t>>& insts, std::uint32_t& cleared,
        std::uint32_t& stale, std::string& error);
    // The checks of ClearRelocateMarks alone (FX_MOTION_RULE at playback begin).
    bool RelocateMarkReady(std::string& error);
    // The entity's draw data the attach update
    // tests (vfunc +0xA8, 0x1405B94: [[entity+0x28]+8]), guarded read.
    bool ReadEntityDrawData(const void* entity, bool& present);
    // The replay's own placement scope (the placement marks its entity);
    // RelocateSkipped reads 0.
    class PlaceScope
    {
    public:
        explicit PlaceScope(const void* entity);
        ~PlaceScope();
        PlaceScope(const PlaceScope&) = delete;
        PlaceScope& operator=(const PlaceScope&) = delete;
    };
    std::uint64_t RelocateSkipped();
    // A detour on the vfx update (FxPauseFreeze)
    // 0x145C17C (one direct caller 0x145C15A, step in xmm1) that passes a
    // step of 0 while the replay owns the world and its transport holds
    // (paused). Offline: the caller 0x145C0EC itself passes 0 (0x145C14D)
    // while the game's own pause flags are set, so 0 is a step the engine
    // feeds it [binary]. With 0 the registry update 0x2C52554 returns early
    // (0x2C52567): no restart, no emission while paused.
    bool InstallPauseFreeze(std::string& error);
    // A true is a lease of 1.5 s the paused
    // playback renews every tick; when it is not renewed the detour lets the
    // engine's step through again by itself (PauseFreezeExpired counts it).
    void SetPauseFreeze(bool frozen);
    std::uint64_t PauseFrozenUpdates();
    std::uint64_t PauseFreezeExpired();

    // Detours on the weapon system's two requests for a shot's effects
    // (weapon_fx_intent_core.h), before the engine's range and camera tests:
    // the muzzle 0x14309A8 (manager, shooter, weapon, weapon object, bone,
    // info, vector*, dword) and the shell 0x14308F0 (manager, shooter, weapon,
    // weapon object, dword, bone). Record only: every call goes to the
    // original unchanged; the arguments land in a ring the script thread
    // reads (CollectWeaponFxSince). Installed once, removed in Shutdown.
    struct WeaponFxCall
    {
        std::uint8_t kind = 0;       // weaponfx::Kind
        std::uint64_t shooter = 0;   // entity address
        std::uint64_t weapon = 0;    // weapon address
        std::uint64_t object = 0;    // weapon object address
        std::int32_t bone = -1;
        std::int32_t argA = 0;
        std::uint32_t fxHash = 0;    // muzzle: [info+0] (guarded read)
        std::uint32_t fxHashAlt = 0; // muzzle: [info+4]
        float range = 0.0f;          // muzzle: [info+8]
        float vec[4] = {};           // muzzle: the vector argument (guarded read)
        bool infoRead = false;
        std::uint32_t threadId = 0;
    };
    bool InstallWeaponFxIntent(std::string& error);
    void CollectWeaponFxSince(std::uint64_t& cursor, std::vector<WeaponFxCall>& out);
    std::uint64_t TotalWeaponFxCalls();
    // The replay calls the same wrapper (the trampoline, past the recording
    // detour) with the arguments weapon_fx_intent_core.h PlanCall built from
    // the puppets. Rung 4: both prologues were checked at
    // install (InstallWeaponFxIntent); no call without them. SEH-guarded;
    // the instance starts and the shell's registered gets the call made on
    // this thread are counted (created_fx). While it runs the thread counts
    // as the replay's for the fx rules (the one-shot owner rule and the
    // suppressed owners leave its effects alone), and NOT for the sound
    // rules (no audio scope: the puppet sound rules judge any sound it
    // makes as the puppet's own).
    struct WeaponFxPlayResult
    {
        bool called = false;
        bool faulted = false;
        std::uint32_t code = 0;
        std::uint64_t rip = 0;        // RVA when inside the game image
        std::uint32_t starts = 0;     // instance starts on this thread during the call
        std::uint32_t startsMuzzle = 0; // of them, of the muzzle site 0x1452671
        std::uint32_t shellGets = 0;  // registered gets of the shell site 0x14513CE on this thread
        std::uint32_t shellCreated = 0; // of them, the ones that made the instance
        const char* why = "none";
    };
    bool CallWeaponFxRequest(std::uint8_t kind, void* shooter, void* weapon, void* object, std::int32_t bone,
        const void* info, const float* vec16, std::int32_t argA, WeaponFxPlayResult& out);
    // Stops an instance the way the engine's registry does (0x29E7C3C, the
    // inst and a byte: 0 lets its particles die off, state 3; 1 kills it,
    // state 4). Offline: the registry update 0x2C52554 calls it with 0 on
    // every entry not got in the frame (0x2C52617, 0x2C52635), and the get
    // core on the old inst of an entry asked for another effect (0x2C49BE6).
    // Prologue checked once; a mismatch disables it (error, no call).
    bool StopInstance(std::uint64_t inst, bool immediate, std::string& error);
    // Writes +0x310 (when not zero) and +0x3C4 (when >= 0) on an instance we
    // own, before its start.
    bool ApplyInstanceExtras(std::uint64_t inst, const float start310[4], float farClip, std::string& error);
    // Playback of a registered effect: the engine's get-or-create by key
    // (owner + id, hash) through the same core the game uses; `created`
    // says whether this call made the instance. Returns 0 without one.
    std::uint64_t RegisteredGetOrCreate(const void* owner, int id, std::uint8_t flag, std::uint32_t effectHash,
        std::uint8_t arg8, std::uint8_t arg9, bool& created, std::string& error);
    // Writes the recorded state into an instance of ours (offset matrix,
    // parent matrix, evolutions, zoom) and starts it when asked.
    // FxRegisteredParentOnce: writeParent false
    // leaves the parent matrix to the engine's attach (an attached instance
    // whose site never sets the parent, 0x146BEE0).
    bool ApplyInstanceState(std::uint64_t inst, const float matrix[16], const float parent[16],
        const std::uint8_t evolutions[kEvolutionBytes], float zoom, bool start, std::string& error,
        bool writeParent = true);
    // Adds the recorded collision planes and the +0x350 vector to an
    // instance of ours through the engine's own functions (signature-checked).
    bool ApplyInstancePlanes(std::uint64_t inst, const float* planes, std::uint32_t planeCount, const float aux350[4],
        std::string& error);
    // Research switch: the registered observer hook is installed disabled and
    // enabled by the bench (fx_registered on|off) so its effect on the game
    // can be measured in one session.
    bool SetRegisteredObserver(bool enabled, std::string& error);
    std::uint64_t TotalRegistered();
    void CollectRegisteredSince(std::uint64_t& cursor, std::vector<RegisteredCall>& out);
    void CollectStartsSince(std::uint64_t& cursor, std::vector<FxStart>& out);
    void Shutdown();
}
