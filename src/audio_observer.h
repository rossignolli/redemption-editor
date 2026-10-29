#pragma once

// Read-only observer of the engine audio entry points (research).
//
// Two detours, both calling the original with the same arguments:
// - the sound init-params constructor, which every sound creation site
//   runs first (579 call sites): records the time and the callers, so the
//   code paths that create sounds around an event (a shot, a hit, a
//   footstep) can be told apart;
// - the speech entity Say(params) (the ambient speech native ends here):
//   records the voice hash, context hash and variation of each line, the
//   fields a replayed line needs, and the callers.
//
// The detours never call natives. Entries are read on the script thread.

#include "foot_event_core.h"
#include "foot_movement_core.h"
#include "foot_material.h"
#include "speech_identity.h"
#include "speech_request.h"

#include <cstdint>
#include <string>
#include <vector>

namespace replay::soundowner
{
    class PacketLives; // sound_owner_table.h
}

namespace replay::audio
{
    // A ped's audio holder and every audio entity inside it lie within this
    // many bytes. The engine allocates holders in blocks of 0x4CB0 (human)
    // and 0x4A70 (horse) (factories 0x89E4F8, 0x89DAC8), so a wider window
    // would take a sound of the next block, another ped, as this ped's at
    // offset +0x4CB0 or +0x4A70; played back it would go to the wrong ped or
    // be refused as unregistered.
    constexpr std::uint64_t kPedAudioSpan = 0x4820;

    constexpr std::uint32_t kCallerDepth = 10;

    struct SoundCall
    {
        std::uint64_t timeUs = 0;
        std::uint32_t callers[kCallerDepth] = {};
    };

    struct SpeechCall
    {
        std::uint64_t timeUs = 0;
        std::uint64_t speechEntity = 0;
        std::uint32_t voiceHash = 0;
        std::uint32_t contextPartialHash = 0; // joaat before finalization
        std::int32_t variation = 0;
        std::uint64_t result = 0;
        std::uint32_t callers[4] = {};
    };

    // One call of the speech core (validation and line creation): the
    // requested context, the result code and the line it created.
    struct SpeechCoreCall
    {
        std::uint64_t timeUs = 0;
        std::uint64_t speechEntity = 0;
        std::uint32_t result = 0;
        std::uint32_t callers[4] = {};
        // Context the request asked for (params +0x150, partial hash).
        std::uint32_t requestedContext = 0;
        // The line the call created (the speaker's line count grew): voice
        // +0x110, context +0x114 and variation +0x118 of its data.
        bool queued = false;
        std::uint32_t lineVoice = 0;
        std::uint32_t lineContext = 0;
        std::int32_t lineVariation = 0;
        // Line state (+0x234) when the core returned, and whether the
        // request scope holds the line (+0x28).
        std::int32_t lineState = -1;
        bool scopeHoldsLine = false;
        // The line itself (matches its start and stop), the voice
        // object it uses (line +0x1E8 = params +0x1C0, offline evidence
        // 0x2FABD23) with its type byte [obj+0] (0x22 voice, 0x21 voice
        // group, ...) and partial name hash [obj+8], and the line data type
        // (data +0xE0 = params +0xF4: 1, 2 or 3).
        std::uint64_t line = 0;
        std::uint64_t voiceObject = 0;
        std::uint8_t voiceObjectType = 0;
        std::uint32_t voiceObjectPartial = 0;
        std::uint32_t dataType = 0;
        // The request fields the line init 0x2FABBAC copied from the resolved
        // params (speech_request.h).
        replay::speech::request::Fields request;
    };
    void CollectSpeechCoreSince(std::uint64_t& cursor, std::vector<SpeechCoreCall>& out);

    // Audible start of a line (0x2FB99E8, state 5): read-only detour that
    // forwards first and records the line with the time it was entered.
    struct SpeechLineStart
    {
        std::uint64_t timeUs = 0;
        std::uint64_t line = 0;
        std::int32_t stateAfter = -1;
        // The line's identity at its start, read after
        // the original returns from the line data (+0x1D8 -> voice +0x110,
        // partial context +0x114, variation +0x118: written only by the data
        // init 0x2FA92C0 at creation; offline 1491.50, see
        // speech_record_match.h). The recorder checks it against the pending
        // line of the same address.
        bool identityRead = false;
        std::uint32_t voice = 0;
        std::uint32_t context = 0;
        std::int32_t variation = 0;
    };
    void CollectSpeechStartsSince(std::uint64_t& cursor, std::vector<SpeechLineStart>& out);
    // False when the start detour is not installed (signature mismatch):
    // lines are then timed by their request.
    bool SpeechStartObserved();

    // One stop of a speech line (research): the reason code, the line state
    // at the stop and who stopped it. FindLineStop returns the latest stop
    // of a line pointer.
    struct SpeechLineStop
    {
        std::uint64_t timeUs = 0;
        std::uint64_t line = 0;
        std::uint32_t reason = 0;
        std::int32_t state = -1;
        std::uint32_t callers[4] = {};
        // What the state 2 update (0x2FB6298) tests before it kills a line with
        // reason 4, read at the stop (offline evidence): the wave slot the
        // reservation 0x2FB7B58 takes (+0xD0, 0 = none), the voice check's pass
        // bit (+0x2BE bit 0, set by 0x2FB7CA0) and the voice object it checks
        // (+0x1E8), the data type (data +0xE0; type 3 needs no slot).
        bool fieldsRead = false;
        std::uint64_t slot = 0;
        std::uint8_t flags2BE = 0;
        std::uint64_t voiceObject = 0;
        std::int32_t dataType = -1;
        // The prepare (0x2FB5BAC -> data vtable +0xC8 = 0x2FB6000) returns 2
        // when the line's sound parent (0x2C3A7CC on line +0xC0) is 0, when
        // the data's sound (data +0xC0) was not created by 0x2FA0100, or when
        // that sound's prepare 0x257CB78 fails (sound +0xEE bit 2: failed).
        bool parentRead = false;
        std::uint64_t parent = 0;
        std::uint64_t dataSound = 0;
        std::uint8_t soundFlagsEE = 0;
        // Closed theater evidence E1/E2: the voice source that chose the slot
        // allocator (line +0x218) and the explicit slot (line +0x208).
        std::int32_t source = -1;
        std::uint64_t explicitSlot = 0;
    };
    bool FindLineStop(std::uint64_t line, SpeechLineStop& out);
    void CollectSpeechStopsSince(std::uint64_t& cursor, std::vector<SpeechLineStop>& out);

    // Explosion audio:
    // every explosion sound request goes through one enqueuer 0x2EF0010
    // (manager 0x5B9FC30, queue of 16 requests of 0x40 B) with no ped owner,
    // which the per-ped sound track never sees (the dynamite would be silent
    // in playback). A read-only detour copies the accepted request and, for a
    // physical explosion, the instance's tag and audio override. Playback
    // goes through 0x2EF01EC, what the native AUDIO_TRIGGER_EXPLOSION does:
    // audio only, no physical explosion.
    struct ExplosionAudioCall
    {
        std::uint64_t timeUs = 0;
        bool hasInstance = false;
        std::int32_t tag = -1;          // instance +0
        std::uint32_t overrideRef = 0;  // instance +0x794
        float position[3] = {};         // request +0x20
        std::uint8_t underwater = 0;    // request +0x30
        std::uint8_t b31 = 0;           // request +0x31
        std::uint8_t b32 = 0;           // request +0x32
        std::uint32_t interior = 0;     // request +0x34
        std::uint32_t caller = 0;       // return address RVA (instance, native, anim event)
    };
    void CollectExplosionsSince(std::uint64_t& cursor, std::vector<ExplosionAudioCall>& out);
    // Animation audio events (diagnostic): a clip's Audio tag queues
    // {extra, hash} on the ped's audio entity (AddAnimEvent 0x2C2D934, queue of
    // 4 at +0x78, count +0xD8) and the ped handler 0x3193CAC plays them. A
    // puppet posed by bones never runs the clip, so these never happen on it.
    // One batch per handler call: the events and how many sounds, footsteps and
    // speech lines the engine created inside it (a batch with none is a
    // candidate for replaying the event itself).
    struct AnimAudioBatch
    {
        std::uint64_t timeUs = 0;
        std::uint64_t audio = 0;
        std::uint64_t owner = 0; // [audio+0x4600], the ped
        std::uint32_t count = 0;
        std::uint32_t hash[4] = {};
        std::int32_t extra[4] = {};
        std::uint32_t sounds = 0;
        std::uint32_t footsteps = 0;
        std::uint32_t speech = 0;
        // Events of the batch whose bytes +8..+0x17 were not zero
        // (the only producer found, the MoVE callback 0x2BC0A7C, writes
        // zeros there and the ped handler 0x3193CAC reads only +4): evidence
        // that the recorded {hash, extra} is the whole payload.
        std::uint32_t tailNonZero = 0;
    };
    void CollectAnimBatchesSince(std::uint64_t& cursor, std::vector<AnimAudioBatch>& out);
    // Queues a recorded animation audio event on a ped's audio
    // entity (its PedAudioHolder, vtables 0x3839538/0x3839630/0x3839728)
    // through the engine's own AddAnimEvent 0x2C2D934 (vtable slot +0xB8),
    // exactly what the MoVE callback 0x2BC0A7C does for a clip's Audio tag;
    // the audio update (0x25338C0 loop, slot +0x30 ProcessAnimEvents
    // 0x30FD6D4) then runs the ped handler 0x3193CAC on it. Checked before
    // the call: the prologue at the RVA, the vtable, its +0xB8 slot, the
    // owner [audio+0x4600] == the ped, [audio+0x14] != 0 (AddAnimEvent's own
    // guard), room in the queue of 4 and no identical event queued. On the
    // script thread only (as PlayFootstep). `status` is queued, not_queued,
    // queue_full, duplicate, inactive, no_audio, vtable_other, slot_other,
    // owner_other, signature_mismatch or exception_XXXXXXXX. `audioOut` is
    // the audio entity used (0 when none).
    bool QueueAnimEvent(void* pedEntity, std::uint32_t hash, std::int32_t extra, std::uint64_t& audioOut,
        std::string& status);
    // The queued event count [audio+0xD8] of an audio entity, -1 when it
    // cannot be read. ProcessAnimEvents sets it back to 0 (0x2C4D5E8) once
    // the handler ran.
    std::int32_t AnimEventCount(std::uint64_t audio);
    // Plays a recorded explosion sound (tag or override ref); false with the
    // reason when the ref cannot be resolved or the signatures differ.
    bool PlayExplosionAudio(const ExplosionAudioCall& call, std::string& error);

    // Footstep entry (0x317A340): the ped footstep audio (holder +0x1560)
    // queues a foot-down event (id, foot < 4, arguments) that its update
    // plays. Observed in every phase; replayed through the original.
    struct FootstepCall
    {
        std::uint64_t timeUs = 0;
        std::uint64_t footAudio = 0;
        std::int32_t eventId = -1;
        std::uint32_t arg3 = 0;
        std::uint8_t foot = 0;
        std::uint8_t arg5 = 0;
        std::uint8_t flag = 0;
        std::uint8_t arg8 = 0;
        std::uint32_t arg7 = 0;
        bool silenced = false; // not played (SetSilencedFootAudios)
        // The main step container the recorded step played with (hash, ref
        // low dword; foot_material.h); 0 = unknown (older clip). PlayFootstep
        // sets it on the puppet's foot before the step plays.
        std::uint32_t materialHash = 0;
        std::uint32_t materialRef = 0;
        // foot_material.h kWetnessBase: the foot's recorded
        // wetness; below 0 = unknown (older clip). PlayFootstep sets it on
        // the puppet's foot, and the movement update entry again.
        float wetness = -1.0f;
        // foot_material.h FootMix: the foot's slot mix (volume,
        // high-pass, low-pass). Record: read at the footstep entry, where the
        // quadruped's hit pattern 0x317AAD0 has just written it. Playback:
        // PlayFootstep sets it on the puppet's foot (ini FootMixReplay).
        footmaterial::FootMix mix;
        // foot_material.h FootDrive: the base volume and the
        // foot's locomotion value the step played with.
        footmaterial::FootDrive drive;
        // The foot's ground layer weights the step played with (recorded from
        // the movement update sample; set on the puppet's foot at the movement
        // update entry), and the holder's audio LOD (+0x45E8) at the foot-down
        // (0xFF unknown).
        bool layersKnown = false;
        float layers[footmaterial::kLayers] = {};
        std::uint8_t audioLod = footmaterial::kLodUnknown;
    };
    void CollectFootstepsSince(std::uint64_t& cursor, std::vector<FootstepCall>& out);
    // foot_material.h: the queued steps the movement update
    // (0x3193D14) is about to play on a foot audio that is not a puppet's,
    // with the slot of their foot at that moment (observer enabled only).
    void CollectFootMaterialSamplesSince(std::uint64_t& cursor, std::vector<footmaterial::Sample>& out);
    // The recorded materials set on the puppets' feet. Cleared at
    // playback start and end; `enabled` false = PlayFootstep does not set
    // them (ini FootMaterialReplay=0). Counters for the playback summary.
    void ClearFootMaterials();
    void SetFootMaterialReplay(bool enabled);
    // foot_material.h kWetnessBase: the recorded foot wetness on
    // the puppets' feet (ini FootWetnessReplay, default 1).
    void SetFootWetnessReplay(bool enabled);
    bool FootWetnessReplay();
    // The recorded per-foot mix on the puppets' feet (foot_material.h FootMix;
    // ini FootMixReplay, default 1; 0 = slots left at the reset values).
    void SetFootMixReplay(bool enabled);
    // foot_material.h FootDrive: ini FootBaseVolumeReplay and
    // FootImpactReplay (defaults 1 and 0: the impact had no measured effect).
    void SetFootDriveReplay(bool base, bool impact);
    bool FootMixReplay();
    bool FootMaterialReplay();
    struct FootMaterialStats
    {
        std::uint64_t requested = 0;     // replayed steps with a recorded material
        std::uint64_t written = 0;       // slot written on the script thread
        std::uint64_t same = 0;          // the slot already held it
        std::uint64_t noContainer = 0;   // 0x252B194 found no container for the ref
        std::uint64_t badFoot = 0;       // foot index >= 4, foot vtable mismatch or a fault
        std::uint64_t detourWrites = 0;  // slot set again at the movement update entry
        std::uint64_t samples = 0;       // record: movement update samples taken
        // The foot wetness (foot_material.h kWetnessBase).
        std::uint64_t wetRequested = 0;  // replayed steps with a recorded wetness
        std::uint64_t wetWet = 0;        // ... above 0 (the wet layer plays)
        std::uint64_t wetWritten = 0;    // foot value written on the script thread
        std::uint64_t wetSame = 0;       // the foot already held it
        std::uint64_t wetBadFoot = 0;    // foot index, vtable or a fault
        std::uint64_t wetDetourWrites = 0; // set again at the movement update entry
        // The per-foot mix (FootMix).
        std::uint64_t mixRequested = 0;   // replayed steps with a recorded mix
        std::uint64_t mixWritten = 0;     // slot written on the script thread
        std::uint64_t mixSame = 0;        // the slot already held it
        std::uint64_t mixBadFoot = 0;     // foot index, vtable or a fault
        std::uint64_t mixDetourWrites = 0; // set again at the movement update entry
        std::uint64_t baseApplied = 0;     // FootDrive: slot volume moved by the recorded base
        std::uint64_t impactWritten = 0;   // FootDrive: the locomotion value written
        std::uint64_t impactDetourWrites = 0;
        // The ground layers (foot_material.h kLayerBase).
        std::uint64_t layerRequested = 0;    // replayed steps with recorded layers
        std::uint64_t layerWritten = 0;      // weights written on the script thread
        std::uint64_t layerSame = 0;         // the foot already held them
        std::uint64_t layerBadFoot = 0;      // foot index, vtable or a fault
        std::uint64_t layerDetourWrites = 0; // set again at the movement update entry
    };
    FootMaterialStats GetFootMaterialStats();
    // The recorded ground layers on the puppets' feet (ini FootLayerReplay,
    // default 1; 0 = the puppet's own layers).
    void SetFootLayerReplay(bool enabled);
    bool FootLayerReplay();
    // The audio LOD override of a puppet holder set to `lod` (0..3; 4 = no
    // override), the generalisation of SetPuppetAudioLodOverride; and the
    // foot's max LOD (foot +0x28).
    bool SetPuppetAudioLodValue(void* pedEntity, std::uint8_t lod, std::string& status);
    bool PuppetFootMaxLod(void* pedEntity, std::uint8_t& maxLod);
    // The foot audios (holder +0x1560) whose own foot-downs the
    // detour keeps from the engine (empty = none): the puppets and the hidden
    // player while a clip plays. PlayFootstep is not affected (it calls the
    // original). SilencedFootsteps counts the foot-downs kept since start.
    void SetSilencedFootAudios(const std::vector<std::uint64_t>& feet);
    std::uint64_t SilencedFootsteps();
    // puppet_audio_rule.h: the ped audio holders (span
    // kPedAudioSpan) whose own create-and-play one-shots the core detour
    // refuses while a clip plays (empty = none): the puppets, the hidden
    // player and his mount. The replay's own sounds pass: the script thread
    // holds a ReplayEmitScope while it plays the clip's events, and the
    // sounds of a footstep or melee call the replay runs pass too.
    void SetSilencedPuppetAudios(const std::vector<std::uint64_t>& holders);
    // The same list with the clip entity of each holder (the puppet's packed
    // replay id; puppetaudio::kNoOwner for the hidden player and his mount),
    // for the persistent rule by entity, call site and entry.
    void SetSilencedPuppetAudios(const std::vector<std::uint64_t>& holders, const std::vector<std::uint32_t>& owners);
    // puppet_audio_rule.h:
    // the CWeapons ([object + 0x448]) of the weapon puppets whose own
    // create-and-play one-shots are refused like the holders' (span
    // kWeaponOneShotSpan; the clip's SND kind 4 carries the recorded ones).
    // Counted apart in RefusedPuppetWeaponOneShots.
    void SetSilencedPuppetWeaponAudios(const std::vector<std::uint64_t>& weapons);
    std::uint64_t RefusedPuppetWeaponOneShots();
    // The puppets' own persistent sounds without tracker on the
    // silenced holders (the cores by ref and by hash) are refused when their
    // call site is in `callers` (the ESND pedAudio callers of the clip) and
    // the switch is on (ini RefusePuppetOwnPersistent, default 1). The
    // replay's own go through the trampolines and never meet the rule.
    void SetRefusePuppetOwnPersistent(bool enabled);
    bool RefusePuppetOwnPersistent();
    void SetOwnPersistentCallers(const std::vector<std::uint32_t>& callers);
    // puppet_audio_rule.h OwnPersistentCarried: with the switch
    // on (ini PuppetPersistentByEntry, default 1) a puppet's own persistent
    // sound is refused only when `keys` (OwnPersistentKey of each ESND
    // pedAudio sound of the clip: entity, call site, entry) has its own key;
    // the hidden player and his mount keep the call site rule. Off: the
    // call site rule (call site alone) for every holder.
    void SetPuppetPersistentByEntry(bool enabled);
    bool PuppetPersistentByEntry();
    // Returns the count published (at most the table's 4096 keys; the rest
    // are dropped).
    std::size_t SetOwnPersistentKeys(const std::vector<std::uint64_t>& keys);
    std::uint64_t RefusedPuppetOwnPersistent();
    std::uint64_t KeptPuppetOwnPersistent(); // on a silenced holder, not carried by the clip
    // refused ref@caller/frame1/frame2#owner ... ; kept: the same (the two
    // game frames above the call site and the holder's clip entity, "hidden"
    // for the hidden player and his mount).
    std::string PuppetOwnPersistentSamples();
    void BeginPuppetOwnPersistent();
    std::uint64_t RefusedPuppetOneShots();
    // Starts a new sample window (playback start); returns the total so far.
    std::uint64_t BeginRefusedPuppetOneShots();
    // The first 16 refusals of the window (ref@caller RVA), for the log.
    std::string RefusedPuppetOneShotSamples();
    // SoundGenericMode=packets at the ports. On (soundgeneric::BeginPlayback,
    // while the generic player plays in packets mode): every create of the
    // engine's own on a silenced holder is judged at S03, S04, S05 and S06,
    // unless the replay makes it or the speech's sound controller does
    // (puppet_audio_rule.h BlockPuppetCreate). Each own create of a puppet goes
    // to a ring (port 3, 4, 5 or 6; owner: the holder's clip entity, kNoOwner
    // for the hidden player and his mount; caller: the return address RVA)
    // with the port's verdict (soundowner::LifeVerdict: 0 passed, 1 recorded,
    // 2 alternative) and whether it was refused; S03 refuses as it always
    // did, S04, S05 and S06 refuse only the recorded and the alternative ones
    // (SetBlockPacketLives, SoundGenericBlock=packet) or everything
    // (SoundGenericBlock=all, or no lives published).
    struct BlockedPuppetCreate
    {
        std::uint64_t timeUs = 0;
        std::uint64_t ref = 0;
        std::uint32_t owner = 0;
        std::uint32_t caller = 0;
        std::uint8_t port = 0;
        std::uint8_t verdict = 0;
        bool refused = true;
    };
    void SetBlockPuppetSounds(bool enabled);
    // The packet lives the ports judge against (owned by the caller, alive
    // until the next call; null: refuse everything), the SoundGenericBlock
    // key, and the playhead the lives are compared with (the script thread
    // sets it every tick). stepSetGate = SoundStepSetGate (puppet_audio_rule.h
    // RefuseUnmadeReplayedSet): inside a replayed step or melee call, S03
    // refuses a set the owner never made in the clip (verdict Unmade).
    void SetBlockPacketLives(const replay::soundowner::PacketLives* lives, bool blockAll, bool stepSetGate = true);
    std::uint64_t RefusedUnmadeSets();
    void SetBlockPlayheadMs(std::uint32_t playheadMs);
    bool BlockPuppetSounds();
    // In packets mode, a puppet's own persistent create by ref whose set the
    // owner never made in the clip and that the ESND track does not carry is
    // refused (puppet_audio_rule.h RefuseUnrecordedOwnPersistent; ini
    // PuppetUnrecordedPersistent, default 1).
    // Begin resets the counters at a playback start; the line is the
    // evidence (PUPPET_UNRECORDED_PERSISTENT) at its end.
    void SetPuppetUnrecordedPersistent(bool enabled);
    bool PuppetUnrecordedPersistent();
    void BeginUnrecordedPersistent();
    std::string UnrecordedPersistentLine();
    std::uint64_t BlockedPuppetCreates();
    void CollectBlockedPuppetCreatesSince(std::uint64_t& cursor, std::vector<BlockedPuppetCreate>& out);
    // vehicle_sound_rule.h: packets mode for the puppet vehicles and train
    // cars. Their audio entities ([vehicle + 0x900]) with each one's clip
    // entity (empty = none); the switch (ini SoundGenericVehicleBlock, default
    // 1). The census creators ask RefuseVehicleOwnCreate before the original
    // (core 0 = creator A, 1 = creator B; caller: the return address RVA): an
    // own create of a listed vehicle that the recording plays then (verdict
    // Recorded or Alternative, SetBlockPacketLives) is refused the way the
    // creator refuses when its slot pool is full (0 returned, the out slot
    // untouched). Every judged create goes to the blocked ring with port 1 or 2
    // and its verdict.
    void SetSilencedVehicleAudios(const std::vector<std::uint64_t>& audios, const std::vector<std::uint32_t>& owners);
    void SetVehicleOwnBlock(bool enabled);
    bool VehicleOwnBlock();
    bool RefuseVehicleOwnCreate(std::uint8_t core, std::uint64_t audio, std::uint64_t soundRef, std::uint64_t out,
        std::uint32_t caller);
    std::uint64_t VehicleOwnRefused();
    std::uint64_t VehicleOwnPassed();
    // puppet_audio_rule.h RefusePuppetOwnSpeech:
    // the speech entities of the puppets while a clip plays (empty = none),
    // with each one's clip entity. A speech core call on one of them outside
    // the replay's own calls is the puppet's own speech: observed always,
    // refused when SetPuppetOwnSpeechBlock(true) (ini PuppetOwnSpeech).
    void SetPuppetSpeechEntities(const std::vector<std::uint64_t>& entities, const std::vector<std::uint32_t>& owners);
    void SetPuppetOwnSpeechBlock(bool block);
    bool PuppetOwnSpeechBlock();
    // Empty when the block is ready; else why it is off (signature mismatch).
    const char* PuppetOwnSpeechError();
    struct PuppetOwnSpeech
    {
        std::uint64_t timeUs = 0;
        std::uint32_t owner = 0;
        std::uint32_t context = 0; // params +0x150, partial hash
        std::uint32_t callers[3] = {};
        bool refused = false;
        std::uint32_t result = 0;
    };
    std::uint64_t PuppetOwnSpeechSeen();
    std::uint64_t PuppetOwnSpeechRefused();
    // Speech core calls of other speech entities while puppets are listed,
    // and how many of them queued a line (the live world talking).
    std::uint64_t WorldSpeechDuringPlayback();
    std::uint64_t WorldSpeechQueuedDuringPlayback();
    void CollectPuppetOwnSpeechSince(std::uint64_t& cursor, std::vector<PuppetOwnSpeech>& out);
    // speech_replay_rules.h: any thread, lock-free. A listed puppet speech
    // entity, and whether this thread is inside a ReplayEmitScope now.
    bool IsPuppetSpeechEntity(std::uint64_t speechEntity);
    bool ReplayEmittingHere();
    struct ReplayEmitScope
    {
        ReplayEmitScope();
        ~ReplayEmitScope();
        ReplayEmitScope(const ReplayEmitScope&) = delete;
        ReplayEmitScope& operator=(const ReplayEmitScope&) = delete;
    };
    // The footstep audio of a ped (holder +0x1560), 0 when it has none.
    std::uint64_t FootstepAudio(void* pedEntity);
    // Queues a recorded footstep on a ped, as the game's foot-down handler
    // does (only when the footstep audio is enabled, +0x24).
    bool PlayFootstep(void* pedEntity, const FootstepCall& step, std::string& error);

    // The second queue of the foot audio (foot_event_core.h; the hoof that
    // lifts or lands), the entry 0x317A1D0 (foot audio, type, foot, spread,
    // direct) detoured (rung 5, prologue and the generator, drain and store
    // sites checked). Every call is forwarded, except a puppet's own one (a
    // foot audio of SetSilencedFootAudios) while SetFootEventRefuseOwn is on (a
    // clip with section FSEV plays). Each call goes to a ring with what the
    // engine queued: the entry {type, foot} when the count moved, with the foot
    // slot's container (+0x240 + 0x40 * foot) and the foot's rise ([row +0x88
    // component +0x160 + 0x90 * foot]) at that moment.
    struct FootEventCall
    {
        std::uint64_t timeUs = 0;
        std::uint64_t footAudio = 0;
        std::int32_t argType = -1;
        std::uint8_t argFoot = 0;
        std::uint8_t spread = 0;
        std::uint8_t direct = 0;
        std::uint8_t origin = 0;  // footevent::Origin (by the return address)
        bool silenced = false;    // a foot of SetSilencedFootAudios
        bool refused = false;     // not forwarded (the puppet's own, a clip with FSEV)
        bool queued = false;      // the count +0xD4 moved by one
        std::int32_t type = -1;   // the queued entry
        std::uint8_t foot = 0;
        std::uint32_t materialHash = 0; // the foot slot when queued (feet 0..3)
        std::uint32_t materialRef = 0;
        bool impactKnown = false;
        float impact = 0.0f;
    };
    void CollectFootEventsSince(std::uint64_t& cursor, std::vector<FootEventCall>& out);
    // Script thread: refuse the silenced feet's own events (true only while
    // a clip with FSEV plays and FootEventReplay is on).
    void SetFootEventRefuseOwn(bool on);
    struct FootEventStats
    {
        bool installed = false;
        std::string error; // the install's, "" none
        std::uint64_t calls = 0;
        std::uint64_t queued = 0;
        std::uint64_t refused = 0;
        std::uint64_t replayQueued = 0; // PlayFootEvent
        std::uint64_t replayFailed = 0;
    };
    FootEventStats FootEventSnapshot();
    // Queues a recorded secondary step on a ped's foot audio, as the drain of
    // the movement update does (direct 1: straight into the queue, the
    // played at the next movement update), after setting the recorded
    // container on that foot's slot (and the recorded rise, FootImpactReplay)
    // the way PlayFootstep does. Script thread; calls the original entry, so
    // the refusal of the detour does not apply. `status` one of
    // footevent::PlayStatusName.
    bool PlayFootEvent(void* pedEntity, std::int32_t type, std::uint8_t foot, std::uint32_t materialHash,
        std::uint32_t materialRef, bool impactKnown, float impact, std::string& status);
    // foot_event_core.h MotionState: the ped's row +0x88
    // movement component, guarded reads (rung 2, evidence only).
    bool ReadMotionState(void* pedEntity, footevent::MotionState& out);
    // foot_event_core.h MovementWindow: each call of the
    // movement volume 0x31815C4 with its input, the engine's result (before
    // the replay's own correction), the limb component's gate byte (holder
    // +0x774) and the foot audio's +0x504, while SetMovementDetail is on
    // (evidence only; guarded reads in the detour, a ring).
    struct MovementDetailCall
    {
        std::uint64_t timeUs = 0;
        std::uint64_t footAudio = 0;
        std::uint8_t foot = 0;
        float value = 0.0f;
        float result = 0.0f;
        std::int8_t gate = -1; // -1 unread
        bool f504Known = false;
        float f504 = 0.0f;
    };
    void SetMovementDetail(bool on);
    // foot_movement_core.h: with it on, a puppet foot whose ped has a published
    // entry for that foot gets the recorded result instead of the engine's (no
    // MovementTermCorrection on top). The script thread publishes up to
    // RecordedMovementSlots() entries (ped 0 clears one).
    void SetMovementReplay(bool on);
    void PublishRecordedMovement(std::size_t index, std::uint64_t ped, std::uint32_t mask, const float* values);
    std::size_t RecordedMovementSlots();
    struct MovementReplayStats
    {
        std::uint64_t given = 0;
        std::uint64_t unmatched = 0;
        bool on = false;
    };
    MovementReplayStats MovementReplaySnapshot();
    void CollectMovementDetailSince(std::uint64_t& cursor, std::vector<MovementDetailCall>& out);

    // Pins a recorded voice on a ped the way SET_AMBIENT_VOICE_NAME does
    // (handler 0xF73D80): the voice object looked up in the audio metadata
    // store by (hash, recorded type) through 0x252B584, then the engine's
    // own apply 0x2F96CC4 on the speech entity's voice holder (vtable
    // +0x128), which writes holder +0x30. The voice resolver 0x2FA66D0
    // returns holder +0x30 directly, so a request with no voice then uses
    // exactly this voice instead of drawing one from the model's voice
    // group (0x2FA2C54, RNG). `voiceType` 0 means 0x22 (the setter's type).
    // When the typed lookup finds nothing, the recorded object is used if it
    // still has that type and name hash (same session). `status` is one of
    // pinned_lookup, pinned_recorded_object, voice_not_found,
    // no_speech_entity, no_voice_holder, voice_signature_mismatch or an
    // exception text.
    bool PinSpeechVoice(void* pedEntity, std::uint32_t voiceHash, std::uint8_t voiceType,
        std::uint64_t recordedObject, std::string& status);
    // Evidence: a sound ref with 0x800000 in its high dword is
    // indirect, its container looked up by 0x252B194(low dword) in the audio
    // metadata store (type 0x29). "direct", "indirect_found",
    // "indirect_missing" or an error.
    const char* IndirectRefState(std::uint64_t soundRef);
    // Create-and-play calls the engine refused since start (they are not
    // recorded: they made no sound in the game).
    std::uint64_t RefusedPlayRequests();

    // What the direct entry created: the line the scope holds and its
    // identity (data +0x110/+0x114/+0x118, voice object +0x1E8).
    struct PlayedLine
    {
        std::uint64_t line = 0;
        std::uint32_t voice = 0;
        std::uint32_t context = 0;
        std::int32_t variation = 0;
        std::uint64_t voiceObject = 0;
        std::uint8_t voiceObjectType = 0;
        std::uint32_t voiceObjectPartial = 0;
        std::uint32_t dataType = 0;
        // Line +0x218, the voice source (3 bank, 4 ambient, 11 fixed slot...),
        // read when the request returned.
        std::int32_t source = 0;
        // The line's copies of the request, with the line's sound set (data
        // +0xE8).
        replay::speech::request::Fields request;
        // speech_conversation.h: what the request did with the asked sound set
        // (SetStatus) and the ref it wrote.
        std::uint8_t setStatus = 0;
        std::uint32_t setWrittenRef = 0;
        // Compare mask against the expected identity (speech_identity.h).
        std::uint32_t mismatch = 0;
        // The line was stopped (0x2FBCCD4, reason 2) before its start.
        bool stoppedOnMismatch = false;
        bool identityRead = false;
    };

    // Replays a recorded line on a ped: the recorded context and variation
    // through the direct line entry, without the AI Say gating, and no voice
    // in the request (the voice is pinned on the holder, PinSpeechVoice).
    // `result` is the core's code (74 = line queued). With `expected`, the
    // created line is compared with it before the start; on any difference
    // the line is stopped instead of started (silence rather than another
    // phrase). Only params +0x18C is set (skips the recently-played check,
    // which otherwise stops every replayed line in state 3); +0x18D is left 0
    // (it only enables the variation re-draw).
    // `startOffsetMs` > 0 asks the line from that point (speech_resume.h;
    // speech params +0x15C; offline 1491.50: params ctor 0x2FAECDC writes 0
    // there, both copies 0x888218 keep it, the line data init 0x2FA92C0 moves
    // it to data +0x11C, the speech sound's init params +0xE8 at 0x2FA0181,
    // 0x2FB4355 and 0x2FB4B4D, which the sound creator 0x2516CAC reads as the
    // start offset, beside the predelay +0xEC).
    // `source` > 0 is written into params +0xF8 (speech_slots.h; 4: an ambient
    // slot by priority, the line's own wave).
    // `ask` (may be null, speech_request.h) writes the recorded line type into
    // params +0xF4 (the ctor leaves 0 and the validator 0x2F9A7BB writes 2 for
    // 0) and the recorded request fields (volume type +0xDC, audibility +0xE0,
    // +0xD8, +0xE8), after the ctor. An Ask with a sound set
    // (speech_conversation.h) writes params +0xC8 (hash), +0xCC (ref) and
    // +0xC0 (the set object) as the SpeechParams applier 0x2F967E8 does, the
    // object found with its own calls (0x252AD8C, 0x252B408 by hash, 0x252B194
    // by ref); the outcome goes to played.setStatus / setWrittenRef.
    bool PlaySpeechDirect(void* pedEntity, std::uint32_t voiceHash, std::uint32_t contextPartialHash,
        std::int32_t variation, const replay::speech::Identity* expected, std::uint32_t& result, PlayedLine& played,
        std::string& error, std::uint32_t startOffsetMs = 0, std::int32_t source = 0,
        const replay::speech::request::Ask* ask = nullptr);
    // The sound set calls' check (prologues and the applier's call sites), done
    // once; `failed` names the first site that did not match ("none" when all
    // did).
    bool SpeechSoundSetCallsOk(std::string& failed);

    // speech_resume.h ClassifyOldCopy: a line pointer the replay stopped, read
    // directly (it may have left the ped's list): whether it still has the line
    // vtable (0x37FD5D0, read once from the line ctor 0x2F91F90's verified
    // bytes; the pool free writes over it), its state (+0x234) and identity
    // (data +0x110/+0x114/+0x118).
    struct StoppedLineView
    {
        bool read = false;
        bool liveVtable = false;
        std::int32_t state = -1;
        std::uint32_t voice = 0;
        std::uint32_t context = 0;
        std::int32_t variation = 0;
    };
    bool ReadStoppedLine(std::uint64_t line, StoppedLineView& out);

    // An animal vocalization by its context hash, as the native
    // PLAY_ANIMAL_VOCALIZATION's handler asks it (speech entity slot +0x290,
    // animal vtable checked). `result` is the Say code (74 = line queued);
    // false with the reason (signature_mismatch, no_speech_entity,
    // not_animal_speech, slot_other, exception_XXXXXXXX).
    // The hash entry is the animal vtable 0x37FE430's slot +0x288 (0x2FB2B4C);
    // the native's +0x290 hashes a name and jumps there. A refusal carries the
    // vtable RVA found (not_animal_speech:vt_<rva>). With the recorded voice
    // and variation (0 = the animal pipeline picks), params +0x5C = 1 as the
    // native sets it: the gate 0x2FC0088 then skips the AI checks (context
    // probability roll 0x2FC0398, concurrent lines per animal type 0x2FC04B8,
    // lines of priority 2/3 playing 0x2FB12D8, distance to the listener
    // 0x2FA7A70, the rider check) that made the direct path return 64
    // (0x2F9A238: 0x40 on refusal, 0x4A on success).
    bool PlayAnimalVocal(void* pedEntity, std::uint32_t contextPartialHash, std::uint32_t voiceHash,
        std::int32_t variation, std::uint32_t& result, std::string& error);
    // The identity of a line still in the ped's line list
    // (guarded reads of the line and its data, as PlaySpeechDirect reads).
    bool ReadSpeechLineIdentity(void* pedEntity, std::uint64_t line, PlayedLine& out);

    // Stops a line of this ped (0x2FBCCD4) after checking it is still in the
    // ped's line list and not stopped (state 6). False otherwise.
    bool StopSpeechLine(void* pedEntity, std::uint64_t line, std::uint32_t reason, std::string& error);

    // Line state (+0x234) when the line is still in the ped's line list.
    bool SpeechLineState(void* pedEntity, std::uint64_t line, std::int32_t& state);

    // Read-only view of a ped's speech entity (evidence for the replay
    // speech path): the pure ped-audio getter (as the last-played-speech
    // native uses it), the speech entity at +0x48C0 when present, its line
    // list (+0x78, count +0x80) and the first line with the data it points
    // to at +0x1D8. Nothing is called beyond the getter; all reads guarded.
    struct SpeechProbe
    {
        std::uint64_t speechEntity = 0;
        std::uint32_t lastContext = 0; // +0x388, partial hash
        std::uint16_t lines = 0;
        std::vector<std::uint8_t> line;     // first line object, 0x200 bytes
        std::vector<std::uint8_t> lineData; // [line+0x1D8], 0x140 bytes
    };
    bool ProbeSpeech(void* pedEntity, SpeechProbe& out, std::string& error);

    // Weapon fire events: the weapon code hands a 0xA0-byte fire info to an
    // enqueue function (max 20 per frame) that the audio update later plays.
    // Observed read-only while recording; replayed by handing a recorded copy
    // (entity references remapped to the puppets) to the same enqueue.
    constexpr std::size_t kFireInfoBytes = 0xA0;
    struct FireCall
    {
        std::uint64_t timeUs = 0;
        std::uint8_t info[kFireInfoBytes] = {};
    };
    void CollectFires(std::uint64_t fromUs, std::uint64_t toUs, std::vector<FireCall>& out);
    // New fire events since `cursor` (advanced).
    void CollectFiresSince(std::uint64_t& cursor, std::vector<FireCall>& out);
    std::uint64_t TotalFires();
    bool EnqueueFire(const std::uint8_t info[kFireInfoBytes], std::string& error);

    // Player weapon sound on the main character's puppet.
    // Offline, build 1491.50: the weapon audio asks one predicate whether
    // the shooter is the player (0x2F02F8C: a ped whose component row slot
    // +0xB8 has byte +0x40 bit 1 set and bit 0 clear). It is the only input
    // of the player variant: the weapon settings (0x2EF1D78 via 0x2F030C0,
    // and 0x3110424 -> settings 0x6F0783F5), the fire sound choice
    // (0x2EF9F7C r9b), the player volume and tail (0x2EFA7E0 r12b) and the
    // NPC gunfire bookkeeping the fire queue consumer (0x2F0DB9C) skips for
    // the player. Twelve callers, all weapon audio. A puppet is never the
    // player, so its shots would take the NPC variant. A detour on the
    // predicate answers yes for the puppets set below and forwards every
    // other ped to the original untouched.
    bool InstallPlayerWeaponAudio(std::string& error);
    // Puppet ped addresses the weapon audio treats as the player (script
    // thread; empty = none).
    void SetPlayerAudioPeds(const std::vector<std::uint64_t>& peds);
    // The weapon settings a fire event plays are the ones the
    // weapon's audio (CWeapon +0x100) cached when it was set up (0x311971C
    // returns +0x1F8; written by 0x3120F54, which picks the player variant
    // 0x6F0783F5 through the predicate on the weapon's owner). The puppet's
    // weapon is set up before its puppet answers "player", so it keeps the
    // NPC settings (the replay would then take the player's sound slot of the
    // NPC settings: a third sound, louder and brighter than both). This puts the
    // player variant of the same weapon's settings (0x2EF1D04 with
    // 0x6F0783F5) in the cache when it holds the NPC ones; signature
    // checked, SEH guarded. Script thread.
    bool RefreshWeaponAudioSettings(std::uint64_t cweapon, std::uint64_t& before, std::uint64_t& after,
        std::string& error);
    // Read-only replica of the predicate's test (evidence): 1 player, 0 not,
    // -1 unreadable.
    int PedAudioPlayerFlag(void* pedEntity);
    // Mix probe, reads only. The ped audio holder's LOD (+0x45E8) and distance
    // metric (+0x4640), the fields PedAudioState logs.
    // False when the ped has no holder or a read faulted.
    bool PedAudioLod(void* pedEntity, std::uint8_t& lod, float& metric);
    // Read only. The offsets 0x2EF8120 adds to every shot of this ped (the
    // holder above): volume holder+0x1CF0 (float), high-pass holder+0x1CF4
    // holder above): volume holder+0x1CF0 (float), high-pass holder+0x1CF4
    // (int); and the audio flags dword [0x5B9CFC0] (bits 2 to 5 read by the
    // shot and the steps, written through pointers: value unknown offline).
    bool PedGunfightOffsets(void* pedEntity, float& volume, std::int32_t& highPass);
    bool AudioGlobalFlags(std::uint32_t& flags);
    // Whether the replay's player weapon audio detour answers "player" for
    // this ped address (SetPlayerAudioPeds), and the mount detour for this
    // slot +0xA8 component (SetPlayerMountAudioComponents).
    bool PlayerAudioPublished(std::uint64_t ped);
    bool MountAudioPublished(std::uint64_t component);
    struct PlayerAudioStats
    {
        std::uint64_t calls = 0;     // predicate calls (all peds)
        std::uint64_t engineTrue = 0; // the original said yes (the real player)
        std::uint64_t overrides = 0;  // yes for a replay puppet
        bool installed = false;       // the detour is in place
        // Per caller (the twelve known return sites, then "other"): yes
        // answers, engine and override together.
        static constexpr std::size_t kSites = 13;
        std::uint64_t siteTrue[kSites] = {};
    };
    PlayerAudioStats PlayerAudioSnapshot();
    // "0x2EFA8F5:n|..." for the sites with a yes since `since`.
    std::string PlayerAudioSites(const PlayerAudioStats& now, const PlayerAudioStats& since);

    // The player's horse variant of the ped audio (mount_audio.h):
    // a detour on the leaf predicate 0x8B69B0 ("is the entity linked to this
    // slot +0xA8 component the local player", the rider of a horse) answers
    // yes for the published components at the thirteen ped audio sites only;
    // everything else goes to the original untouched. Signature checked with
    // the whole function; `player_mount_audio_signature_mismatch` in the
    // install error when the build differs (nothing changes then).
    bool InstallPlayerMountAudio(std::string& error);
    // Slot +0xA8 components (MountLinkComponent of the horse puppets that
    // carry the player's puppet) the ped audio treats as ridden by the
    // player (script thread; empty = none; up to 4).
    void SetPlayerMountAudioComponents(const std::vector<std::uint64_t>& components);
    // The detour of the (foot_material.h MovementTermCorrection)
    // movement volume 0x31815C4 (installed with the observer). The script
    // thread sets whether it acts (ini MovementVolumeReplay while a clip
    // plays) and whether the hidden live player rides.
    bool InstallMovementVolume(std::string& error);
    void SetMovementTermState(bool enabled, bool livePlayerMounted);
    struct MovementTermStats
    {
        bool installed = false;
        std::uint64_t calls = 0;       // puppet-relevant calls while enabled
        std::uint64_t given = 0;       // term given back
        std::uint64_t givenPlayer = 0; // ... on the player's puppet
    };
    MovementTermStats MovementTermSnapshot();
    // The slot +0xA8 component of a ped (0 when unreadable).
    std::uint64_t MountLinkComponent(void* pedEntity);
    // Read-only replica of the predicate for a ped (evidence): 1 when the
    // linked entity is the local player, 0 not or none, -1 unreadable;
    // `linked` is the entity it reads.
    int MountLinkIsPlayer(void* pedEntity, std::uint64_t& linked);
    struct MountAudioStats
    {
        std::uint64_t calls = 0;      // predicate calls (every caller)
        std::uint64_t engineTrue = 0; // the original said yes (a horse the player rides)
        std::uint64_t overrides = 0;  // yes for a replay horse puppet
        bool installed = false;
        // Per caller: the thirteen ped audio sites, then "other".
        std::uint64_t siteTrue[14] = {};
    };
    MountAudioStats MountAudioSnapshot();
    std::string MountAudioSites(const MountAudioStats& now, const MountAudioStats& since);

    // Pure ped audio getter (0 when absent).
    std::uint64_t PedAudio(void* pedEntity);

    // Speech entity of a ped the way the ambient speech native reaches it:
    // the ped's component at table slot +0x30 (read, never created), then
    // [[component+0x98]+0x4438] when component+0x218 has bit 1. 0 when absent.
    std::uint64_t SpeechEntity(void* pedEntity);
    // The ped audio object the speech entity hangs from (+0x4438), 0 when
    // the ped has none. Same component path as SpeechEntity.
    std::uint64_t PedAudioHolder(void* pedEntity);

    // Guarded 8-byte read of engine memory (false on fault).
    bool ReadU64(std::uint64_t address, std::uint64_t& value);
    // A static audio entity of the image a clip names (SNDS globalAudio, ESND
    // audioRva) as an absolute address: true only for one of
    // census::kImageAudio whose live vtable is still the recorded one. False
    // with status unknown_image_audio(0x<rva>) or image_audio_vtable_mismatch(0x<rva>).
    bool TrustedImageAudio(std::uint64_t address, std::string& status);
    // Guarded copy of engine memory (false on fault).
    bool ReadBytes(std::uint64_t address, void* out, std::size_t size);


    // Research: calls into the melee impact audio entry 0x318A28C(entity,
    // info), with its arguments and the first 0x100 bytes of the info.
    struct MeleeCall
    {
        std::uint64_t timeUs = 0;
        std::uint32_t entry = 0; // 1: 0x318A28C
        std::uint64_t args[4] = {};
        std::uint8_t info[0x100] = {};
        std::uint32_t callers[3] = {};
    };
    void CollectMelee(std::uint64_t fromUs, std::uint64_t toUs, std::vector<MeleeCall>& out);
    void CollectMeleeSince(std::uint64_t& cursor, std::vector<MeleeCall>& out);

    // Research: the melee body impact sound 0x318EC48 (called by damage
    // code at the hit; ten arguments: melee audio holder +0x1B60, impact
    // position, two objects, then six stack arguments of which the 7th is
    // a float). The detour forwards all ten untouched.
    struct ImpactCall
    {
        std::uint64_t timeUs = 0;
        std::uint64_t args[10] = {};
        std::uint8_t position[16] = {};
        std::uint8_t object8[0x40] = {};
        std::uint8_t object9[0x40] = {};
        // The 6th argument: the melee definition (vtable +0x00, hash +0x48).
        std::uint64_t definitionVtable = 0;
        std::uint32_t definitionHash = 0;
        std::uint32_t callers[3] = {};
    };
    void CollectImpacts(std::uint64_t fromUs, std::uint64_t toUs, std::vector<ImpactCall>& out);
    void CollectImpactsSince(std::uint64_t& cursor, std::vector<ImpactCall>& out);

    // Replays a recorded body impact (both peds' impact sounds) through the
    // same function: the puppet's melee audio, the recorded position, the other
    // puppet (or none), the recorded values, the definition when it is still
    // the same object, the recorded strength; the two optional objects (8th,
    // 9th) are left empty. Validated before the call.
    struct BodyImpact
    {
        float position[4] = {};
        std::uint64_t arg4 = 0;
        std::uint64_t arg5 = 0;
        std::uint64_t definition = 0;
        std::uint64_t definitionVtable = 0;
        std::uint32_t definitionHash = 0;
        std::uint64_t arg7 = 0;
        std::uint64_t arg10 = 0;
    };

    // Generic ped sound requests: the deferred create-and-play entry 0x310E680
    // (audio entity, 64-bit sound ref, owner entity, init params or null, four
    // byte flags) queues a one-shot sound the audio update creates later.
    // Recorded when the owner is a recorded ped; replayed on the puppet through
    // the original with the params rebuilt (value fields only; tracker, wave
    // slot, category, environment group and the +0x12C handle left to the
    // engine).
    constexpr std::size_t kSoundParamsBytes = 0x140;
    struct SoundRequest
    {
        std::uint64_t timeUs = 0;
        std::uint64_t audioEntity = 0;
        std::uint64_t soundRef = 0;
        std::uint64_t owner = 0;
        // Made inside a footstep or melee action call (replayed by those).
        bool inReplayedSystem = false;
        bool hasParams = false;
        std::uint8_t params[kSoundParamsBytes] = {};
        std::uint64_t flags[4] = {};
        std::uint32_t callers[4] = {};
        // Persistent creation only: the out slot and the sound it received.
        std::uint64_t slot = 0;
        std::uint64_t sound = 0;
    };
    void CollectSoundRequestsSince(std::uint64_t& cursor, std::vector<SoundRequest>& out);
    // Direct create-and-play requests (0x2589A40: audio entity, ref,
    // params); owner is 0 (attributed by audio entity or tracker).
    void CollectPlayRequestsSince(std::uint64_t& cursor, std::vector<SoundRequest>& out);
    // The ped's sound tracker (what the engine stores at params +0xC8).
    std::uint64_t PedSoundTracker(void* pedEntity);
    std::uint64_t PedAudioComponent(void* pedEntity);
    // The ped's component table row (slot +0x30 is the audio component).
    std::uint64_t PedComponentRow(void* pedEntity);
    // One log field list with the audio registration and foot component
    // state of a ped (evidence, reads only).
    std::string PedAudioState(void* pedEntity);
    // Puppets only: sets the foot component's force bit so the
    // puppet's footstep audio skips the engine's two-per-category table
    // (see audio_observer.cpp). Guarded write after a vtable check; status
    // "already" when the bit is set (called every tick: a puppet's audio
    // holder can be rebuilt after the spawn).
    bool ForceFootstepAudio(void* pedEntity, std::string& status);
    // Puppets only.
    // `on`: the holder's audio LOD override (foot+0x444 =
    // holder+0x19A4) set to 0, so the engine's LOD from the distance to the
    // listener (the free camera in a replay) no longer turns the puppet's
    // foot component off while recorded steps are due; false writes 4 (no
    // override, the constructor's value) back. Vtable checked, guarded;
    // status "already" when the value is there.
    bool SetPuppetAudioLodOverride(void* pedEntity, bool on, std::string& status);
    // Replays a direct create-and-play request: on the puppet's ped audio
    // (+offset) when `global` is 0, else on that static audio entity with
    // the puppet as tracker; params rebuilt as for PlaySoundRequest.
    // Persistent collision sounds (scrape/roll: created on the static
    // collision audio by 0x2588FE8, driven every frame, stopped with
    // StopAndForget 0x2585C2C). Recording tracks each created sound and its
    // stop; playback creates its own into a slot it owns, plays it, drives
    // its variables and position, and stops it.
    struct SoundStop
    {
        std::uint64_t timeUs = 0;
        std::uint64_t sound = 0;
    };
    void CollectSoundStopsSince(std::uint64_t& cursor, std::vector<SoundStop>& out);
    std::uint64_t StaticCollisionAudio();
    bool ReadSoundVariable(std::uint64_t sound, std::uint32_t hash, float& value);
    bool WriteSoundVariable(std::uint64_t sound, std::uint32_t hash, float value);
    // S38 with its verdict: "ok", or why it was not set
    // (no_sound, no_settings, exception, bad_value, unchecked, no_pool,
    // unreadable, no_block, no_block_on_parent, bad_block, empty_block,
    // absent: the sound's tree exports no such variable, refused_found).
    const char* WriteSoundVariableWhy(std::uint64_t sound, std::uint32_t hash, float value);
    const char* SoundVariableRefusal(std::uint64_t sound, std::uint32_t hash, float value);
    bool SetSoundPosition(std::uint64_t sound, const float position[4]);
    // Creates and plays a persistent sound into `*slot` (which must stay at
    // a stable address; the engine nulls it when the sound ends).
    bool StartPersistentSound(std::uint64_t audio, std::uint64_t soundRef, const std::uint8_t* params,
        std::uint64_t* slot, std::string& error);
    // StopAndForget; the engine nulls `*slot`.
    void StopPersistentSound(std::uint64_t* slot);

    // Sounds the engine ties to an entity through its tracker
    // (init params +0xC8 = entity + offset): the persistent create cores by
    // ref (0x2589124) and by hash (0x2589058), read-only detours. The lit
    // molotov's loop on the flying bottle comes this way (0x2F2A2C8: the
    // per-object loop list 0x2F10A68/0x2F20D8C, the bottle as tracker at
    // +0x450, the static audio 0x5B9F960). They are recorded with the entity
    // and replayed on its puppet. Creates made inside the collision entry
    // 0x2588FE8 are left to the collision track (return address 0x2589030).
    struct EntitySoundCreate
    {
        std::uint64_t timeUs = 0;
        std::uint64_t audio = 0;
        std::uint64_t soundRef = 0; // the u64 ref, or the u32 hash when byHash
        bool byHash = false;
        std::uint64_t slot = 0;     // audSound** the engine owns
        std::uint64_t sound = 0;    // *slot after the create
        std::uint64_t tracker = 0;  // params +0xC8
        bool hasParams = false;
        std::uint8_t params[kSoundParamsBytes] = {};
        std::uint32_t caller = 0;   // return address RVA
    };
    void CollectEntitySoundsSince(std::uint64_t& cursor, std::vector<EntitySoundCreate>& out);
    // Creates (by ref or hash) and plays a recorded entity sound into `*slot`
    // on `audio` with the recorded params (tracker cleared: the caller
    // drives the position); signature-checked, SEH-guarded.
    bool StartEntitySound(std::uint64_t audio, std::uint64_t soundRef, bool byHash, const std::uint8_t* params,
        std::uint64_t* slot, std::string& error);

    // A weapon object's weapon (object +0x448, offline evidence: the body
    // impact code 0x3190348 plays the carried weapons' rattle on its +0x100
    // audio through 0x2588794); 0 when the object has none.
    std::uint64_t WeaponAudio(void* weaponObject);
    // `envGroup` goes in the params' environment group (+0xE0), which the
    // recorded params copy leaves clear (CopyRecordedSoundParams skips [0xC8,
    // 0xE8)); 0 keeps it clear.
    bool PlayDirectSound(void* pedEntity, std::uint64_t audioOffset, std::uint64_t global,
        std::uint64_t soundRef, const std::uint8_t* params, bool trackPed, std::string& error,
        std::uint64_t envGroup = 0);
    // An entity's environment group through 0xE383E8 (the call the ped request
    // queue makes for a request's owner), script thread, signature checked,
    // SEH; status env_given / env_none / env_*.
    std::uint64_t EnvironmentGroupOf(void* entity, const char*& status);
    // `audioOffset` is the recorded audio entity relative to the owner's
    // ped audio holder (0 for the holder itself).
    bool PlaySoundRequest(void* pedEntity, std::uint64_t audioOffset, std::uint64_t soundRef,
        const std::uint8_t* params, const std::uint64_t flags[4], std::string& error);
    // The ped's melee audio (holder +0x1B60), the entry's first argument.
    std::uint64_t MeleeAudio(void* pedEntity);
    // Plays a recorded melee impact on a ped through the same entry, after
    // checking the melee audio belongs to that ped. The recorded definition
    // (the engine's action result) is used when it is still the same object
    // (same vtable and hash); otherwise (a clip from disk) the engine's
    // action result for the recorded name hash (status "lookup"). When that
    // lookup fails the hit is refused (status refused_no_action_result(..)):
    // the recorded vtable is clip data and never becomes an object's class.
    bool PlayMeleeImpact(void* pedEntity, std::uint64_t info, std::uint64_t infoVtable,
        std::uint32_t infoHash, std::string& error);

    // Research (molotov, read-only): the engine's positional
    // emitter sounds (manager 0x5C9A660, 128 slots of 0x1A0 at +0x1D0, the
    // double-buffer index at +0xD1F8). The particle audio of an effect
    // instance (the ptfx callback 0x1418490 -> 0x3212028: the lit rag and
    // the flight trail of the molotov) and keyed users (0x320F8FC) start and
    // move their sound through a slot; its tick 0x320E568 creates the sound
    // (+0x180). Offline evidence in the build 1491.50 dump; the manager
    // address is taken from the checked site 0x14184B4.
    struct EmitterSlot
    {
        std::uint8_t index = 0;
        bool active = false;      // +0x190
        bool stopping = false;    // +0x175
        bool sound = false;       // +0x180 != 0
        std::uint64_t desc = 0;   // +0x160 sound descriptor
        std::uint64_t key = 0;    // +0x158
        std::uint64_t entity = 0; // +0x150 entity ref
        std::uint32_t hash = 0;   // +0x168 name hash (particle path)
        float position[3] = {};   // +0x00/+0x10 by the buffer index
    };
    // All active slots; false when the manager site does not match.
    bool SampleEmitterSlots(std::vector<EmitterSlot>& out, std::string& error);

    bool Install(std::string& error);
    void SetEnabled(bool enabled);
    // How deep the sound creation detour walks the stack. Its callers only feed
    // logs: the research windows (POSE_V2_AUDIO_PATH, all kCallerDepth) and,
    // with 3, POSE_V2_AUDIO_TOP and the sounds of a replay call. Full depth
    // only while the research windows are on; 3 otherwise (the walk is 3 us of
    // 3.1 per sound in a recording, ~40 us per frame in a busy scene), and none
    // with Debug=0 (the log drops those lines).
    void SetFullSoundCallers(bool full);
    // horse_audio_evidence.h: the direct create-and-play ring
    // (CollectPlayRequestsSince) also records outside SetEnabled, so a
    // playback's sounds can be compared with the recording's. Evidence only.
    void SetPlayRequestEvidence(bool on);
    // The mix probe's own claim on the same ring (MixProbe=1), apart from the
    // horse evidence's, which turns its own off.
    void SetPlayRequestMixProbe(bool on);
    // While a playback runs, the entity creates (by ref and by
    // hash, with or without tracker) go to the entity sound ring without
    // stop tracking, so the playback sees what its puppets make by themselves.
    void SetObserveEntityCreates(bool observe);
    std::uint64_t NowUs();
    void CollectSounds(std::uint64_t fromUs, std::uint64_t toUs, std::vector<SoundCall>& out);
    void CollectSpeech(std::uint64_t fromUs, std::uint64_t toUs, std::vector<SpeechCall>& out);
    std::uint64_t TotalSounds();
    std::uint64_t TotalSpeech();
    // joaat finalization of a partial hash (as the engine does it).
    std::uint32_t FinalizeHash(std::uint32_t partial);

    // The ports the generic sound player calls, the same functions the game
    // uses, each resolved once: through the read probe's trampoline when that
    // port is hooked, else at its address after its signature matched (a
    // masked signature, plus the second site check S13 has). A port that does
    // not match is never called (false, status "port_unverified"). All are
    // called from the script thread, one of the threads the game calls them
    // from.
    //
    // The init params of a generic packet (sound_generic_core.h InitParams:
    // the fields the recorder kept, not the 0x140 bytes), written over the
    // constructor's defaults.
    struct GenericSoundInit
    {
        bool hasPosition = false;
        float position[3] = {};
        float volume = 0.0f;
        float volume2 = 0.0f;
        std::int16_t pitch = 0;
        std::int16_t pan = -1;
        std::uint8_t flags = 0x20;
        std::uint8_t variableCount = 0;
        std::uint32_t variableHash[3] = {};
        float variableValue[3] = {};
        // The init variables (params +0x10, count +0x90).
        std::uint8_t initVariableCount = 0;
        std::uint32_t initVariableHash[16] = {};
        float initVariableValue[16] = {};
        // The value dwords the game set (sound_generic_core.h ValueDword;
        // index = offset / 4), written over the constructor's before the
        // index = offset / 4), written over the constructor's before the
        // fields above; and the entity whose environment group the sound
        // gets (0: none), as the ped request queue gives it (0x312D9B6).
        std::uint8_t valueCount = 0;
        std::uint8_t valueIndex[35] = {};
        std::uint32_t valueRaw[35] = {};
        std::uint64_t envGroupEntity = 0;
        // out: the environment group given (0 none), and why not.
        std::uint64_t envGroupGiven = 0;
        const char* envGroupStatus = "not_asked";
        // SoundGenericDirectParams=1: the direct create (modes a and b) takes the
        // params whole from the track row that kept them (SNDS, ESND, capture),
        // as mode c's queue does (CopyRecordedSoundParams: the known category
        // at +0xD8 included), then the flags above and the environment group.
        const std::uint8_t* recordedParams = nullptr;
        // SoundGenericDirectParams=2: only the category pointer (+0xD8) of
        // that row over the packet's fields (written when this process knows it).
        std::uint64_t category = 0;
        // SoundGenericDirectParams=3: the row's dword list +0x10C..+0x127
        // (with its count at +0x124) over the packet's fields.
        const std::uint8_t* listParams = nullptr;
    };
    // The requested settings of a live sound the replay
    // reads back after creating it (proven fields, dump 1491.50): +0x22C the
    // client variable (written by 0x25363F8 from params +0xF8, read by the
    // horse's UpdateSound 0x31A56F8), +0x248 the environment group (S30
    // 0x2536948), +0x260 bit 0x10 the sound updates with its entity
    // (0x258AA40 from params flags 0x04; 0x2585E4C calls the entity).
    struct GenericSettingsView
    {
        std::uint32_t clientVariable = 0;
        std::uint64_t envGroup = 0;
        bool updatesWithEntity = false;
    };
    bool ReadGenericSettings(std::uint64_t sound, GenericSettingsView& out);
    // Creates the sound on `audio` (checked as an audio entity object, as
    // StartEntitySound) into `*slot`; plays it (S11) when `play`, else
    // leaves it created for PrepareSound / PlaySound.
    bool CreateGenericSound(std::uint64_t audio, std::uint64_t soundRef, bool byHash, GenericSoundInit& init,
        bool play, std::uint64_t* slot, std::string& error);
    // SoundGenericEntityMode=c: the request through the game's own queue (S03,
    // the public 0x2588950 into 0x2589A40): the recorded params whole when
    // `recorded` (kSoundParamsBytes), else the constructor's with `init`'s
    // fields; no tracker. The sound the queue makes later for (audio, ref)
    // passes the packets mode's port block once (within 2 s). True when the
    // queue took it ("queued"). `*ticket` (when given) names the request; the
    // creator's sound for it comes back by CollectQueueBirthsSince. With
    // SetQueueEnvGroup(true) the group of init.envGroupEntity goes in +0xE0
    // (status and group written back to init), as CreateGenericSound does.
    bool QueueGenericSound(std::uint64_t audio, std::uint64_t soundRef, const std::uint8_t* recorded,
        GenericSoundInit& init, std::string& error, std::uint64_t* ticket = nullptr);
    void SetQueueEnvGroup(bool on);
    // The creator (census S01/S02, any thread) hands the sound it made for
    // (audio, ref) to the oldest waiting ticket of that pair (2 s window); the
    // player drains the births by ticket on the script thread. A cancelled
    // ticket takes no sound.
    struct QueueBirth
    {
        std::uint64_t ticket = 0;
        std::uint64_t sound = 0;
        std::uint64_t out = 0;
        std::uint64_t bornUs = 0;
    };
    void NoteQueueBirth(std::uint64_t audio, std::uint64_t soundRef, std::uint64_t sound, std::uint64_t out);
    // Creates on a silenced puppet's audio no ticket took
    // (count since the reset, the first 8 as owner@ref).
    std::uint64_t UnticketedPuppetCreates(std::string& samples);
    void ResetUnticketedPuppetCreates();
    void CancelQueueTicket(std::uint64_t ticket);
    std::uint64_t QueueBirthCursor();
    void CollectQueueBirthsSince(std::uint64_t& cursor, std::vector<QueueBirth>& out);
    // S10 prepare (sound, no wave slot, may load, priority 0): the preload.
    bool PrepareSound(std::uint64_t sound, std::string& error);

    // The dynamic wave bank a sound ref needs and whether a wave slot holds it
    // now, read through the game's own functions (read only). 0x2EF341C(u32*
    // bank, ref, u32* count): the banks of the sound's hierarchy that no static
    // slot holds (the first in *bank, how many in *count); 0x2545B1C(bank): the
    // wave slot holding the bank (the bank map, under its lock), else null;
    // 0x2549530(slot, bank): that slot's load status (0 loaded, 1 loading, 2
    // failed, 3 not requested). A sound created with AllowLoad off and no slot
    // (the anim-event foley, 0x318B970) only plays when a slot already holds
    // its bank: the simple-sound prepare 0x2566510 fails otherwise and the
    // audio update frees it.
    struct BankProbe
    {
        std::uint32_t bank = 0xFFFFFFFFu; // the first dynamic bank, 0xFFFFFFFF none
        std::uint32_t count = 0;          // dynamic banks of the sound (0: all static)
        bool slotFound = false;           // a wave slot holds the bank now
        std::uint32_t status = 0xFFFFFFFFu; // that slot's load status
    };
    bool ProbeSoundBank(std::uint64_t soundRef, BankProbe& out, std::string& error);
    const char* BankStateName(const BankProbe& probe); // static, absent, loaded, loading, failed, not_requested, unknown
    // S11 prepare and play on a created sound (the mod's own call shape).
    bool PlaySound(std::uint64_t sound, std::string& error);
    // S13 stop (the sound keeps its reference; the engine releases it).
    // Every port on a raw sound (S10, S11, S13, the setting ports, S16, S38,
    // the settings read) first checks the engine's back reference
    // (SoundIdentity) and refuses a freed or reused sound ("sound_gone")
    // without calling the engine.
    bool StopSound(std::uint64_t sound, std::string& error);
    // A sound pointer the engine keeps no slot for (mode c's queued sounds)
    // is held with its identity: the requested settings block of the live
    // top-level sound (vtable slot 1 is S38, sound+0xD6 names the block, the
    // block points back at the sound). 0 when the sound is freed or reused.
    // SoundIdentityHolds: the same sound still owns the same block.
    std::uint64_t SoundIdentity(std::uint64_t sound);
    bool SoundIdentityHolds(std::uint64_t sound, std::uint64_t identity);
    // shot_audio_delay.h: the play state of a live top-level sound, the two top
    // bits of +0xF2 (offline 1491.50: the requested-settings processor
    // 0x2533EF0 reads them at 0x2533F72; the prepare writes 1, the play
    // 0x258791C writes 2, 0x25878E4 writes 3). False (state -1) when the sound
    // no longer holds `identity` (freed or reused); guarded read, script
    // thread.
    bool SoundPlayState(std::uint64_t sound, std::uint64_t identity, int& state);
    enum SoundLinkPort : int
    {
        kLinkStop = 0,
        kLinkPrepare,
        kLinkPlay,
        kLinkAdjust,
        kLinkVariable,
        kLinkRead,
        kSoundLinkPorts
    };
    struct SoundLinkStats
    {
        std::uint64_t checks = 0;
        std::uint64_t notSound = 0;   // vtable slot 1 is not S38 (freed, reused)
        std::uint64_t noSettings = 0; // index 0xFFFF (released) or unreadable
        std::uint64_t broken = 0;     // the block names another sound
        std::uint64_t refused[kSoundLinkPorts] = {};
        std::uint64_t persistentBroken = 0; // a slot the engine still holds failed the check (expected 0)
    };
    SoundLinkStats SoundLinkCounters();
    // The requested settings of a live sound (S18, S19, S21, S22, S23, S24).
    bool SetSoundVolume(std::uint64_t sound, float volume);
    bool SetSoundPitch(std::uint64_t sound, std::int16_t pitch);
    bool SetSoundLowPass(std::uint64_t sound, std::uint32_t cutoff);
    bool SetSoundHighPass(std::uint64_t sound, std::uint32_t cutoff);
    bool SetSoundPostSubmix(std::uint64_t sound, float attenuation);
    bool SetSoundVolumeCurveScale(std::uint64_t sound, float scale);
    // "S10:ok;S13:ok;S18:ok;..." after the ports were resolved (resolves them).
    std::string GenericSoundPortStatus();
    void Shutdown();
}
