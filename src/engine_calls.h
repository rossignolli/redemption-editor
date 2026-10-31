#pragma once

// Every engine function the mod calls, declared once with its type, and the
// one guarded call they all go through. A signature proves a function's
// bytes, not what it takes: the type here is what every call passes, and
// tools/sigs/sigtool.py calls checks that the function's signature covers
// the code that reads each of these parameters. The name is the signature's.
//
// A fault in the call is caught here, counted per function, and its first
// one logged (PollFaults); the caller gets false and goes on as it did when
// its own guard caught the fault. Not for the trampolines of the mod's own
// detours: those pass the engine's own call through as the engine made it.

#include <excpt.h>

#include <cstdint>
#include <ostream>
#include <type_traits>

namespace replay::engine
{
    // An engine function: the name of its signature, and its type (x64: one
    // calling convention, the Microsoft x64 one the engine is built with).
    template <typename Fn>
    struct Function
    {
        const char* name;
    };

    // A fault of `name` (`exception`: its EXCEPTION_POINTERS): counted, the
    // first per function kept for PollFaults, the code and the faulting
    // address kept for the calling thread. Returns EXCEPTION_EXECUTE_HANDLER.
    int NoteFault(const char* name, void* exception);

    // This thread's last faulted call: its exception code (0: none yet) and
    // the address that faulted.
    std::uint32_t LastFault();
    std::uintptr_t LastFaultAddress();

    // For a guarded caller: `code` gets LastFault(); returns false.
    inline bool Failed(std::uint32_t& code)
    {
        code = LastFault();
        return false;
    }

    // Raises this thread's last fault again (its code), for a body run under
    // a guard of the caller's own that reports it (several calls in one).
    [[noreturn]] void Raise();

    // Script thread: an ENGINE_CALL_FAULT line for each function whose first
    // fault came since the last poll, with its code and count so far.
    void PollFaults(std::ostream& log);

    namespace detail
    {
        template <typename T>
        struct Same
        {
            using type = T;
        };

        // No object with a destructor here (MSVC: __try).
        template <typename R, typename... A>
        bool Guarded(const char* name, std::uintptr_t address, R* out, A... args)
        {
            using Fn = R(__fastcall*)(A...);
            __try
            {
                if constexpr (std::is_void_v<R>)
                    reinterpret_cast<Fn>(address)(args...);
                else
                    *out = reinterpret_cast<Fn>(address)(args...);
                return true;
            }
            __except (NoteFault(name, _exception_info()))
            {
                return false;
            }
        }
    }

    // Calls the function at `address` (the function, or the trampoline of
    // the mod's own detour of it) as `f` declares it, its result in `out`.
    // False when `address` is 0 (nothing called) or the call faulted (`out`
    // unchanged).
    template <typename R, typename... A>
    bool Call(const Function<R(A...)>& f, std::uintptr_t address, R& out, typename detail::Same<A>::type... args)
    {
        return address && detail::Guarded<R, A...>(f.name, address, &out, args...);
    }

    template <typename... A>
    bool Call(const Function<void(A...)>& f, std::uintptr_t address, typename detail::Same<A>::type... args)
    {
        return address && detail::Guarded<void, A...>(f.name, address, nullptr, args...);
    }

    // ---- the functions ----------------------------------------------------
    // By module; each name is its signature's (sigtool calls finds it).

    // audio_observer.cpp: the sound params and their release, the sound
    // requests and creates, the persistent sounds, the generic ports, the
    // banks, the speech (params, scope, direct entry, lines, voices, the sound
    // set), the melee, explosion, fire, footstep and anim event sounds.
    using u64 = std::uint64_t;
    inline constexpr Function<void*(void* params)> kSoundParamsCtor{"audio_sound_params_ctor"};
    inline constexpr Function<void(void* handle)> kSoundParamsRelease{"audio_sound_params_release"};
    inline constexpr Function<bool(u64 audio, u64 soundRef, u64 owner, u64 params, u64, u64, u64, u64)> kSoundRequest{
        "audio_sound_request"};
    inline constexpr Function<bool(u64 audio, u64 soundRef, u64 params)> kCreateAndPlayPublic{
        "audio_create_and_play_public"};
    inline constexpr Function<u64(u64 audio, u64 descriptor, u64 slot, u64 params)> kPersistentCreate{
        "audio_persistent_create"};
    inline constexpr Function<void(u64 audio, u64 soundRef, u64 slot, u64 params)> kPersistentByRef{
        "audio_persistent_by_ref"};
    inline constexpr Function<void(u64 audio, std::uint32_t soundHash, u64 slot, u64 params)> kPersistentByHash{
        "audio_persistent_by_hash"};
    inline constexpr Function<void(u64 sound, u64, std::uint8_t, std::int32_t, std::uint8_t)> kPrepareAndPlay{
        "audio_prepare_and_play"};
    inline constexpr Function<void(u64 sound, std::uint8_t)> kStopAndForget{"audio_stop_and_forget"};
    inline constexpr Function<bool(u64 sound, std::uint32_t hash, float* value)> kGetVariable{"audio_get_variable"};
    inline constexpr Function<void(u64 settings, const float* position)> kSetPosition{"audio_set_position"};
    inline constexpr Function<u64(u64 sound, u64 waveSlot, u64 mayLoad, u64 priority)> kGenericS10{"audio_generic_s10"};
    inline constexpr Function<u64(u64 sound)> kGenericS13{"audio_generic_s13"};
    inline constexpr Function<u64(u64 settings, float volume)> kGenericS18{"audio_generic_s18"};
    inline constexpr Function<u64(u64 settings, u64 pitch)> kGenericS19{"audio_generic_s19"};
    inline constexpr Function<u64(u64 settings, u64 cutoff)> kGenericS21{"audio_generic_s21"};
    inline constexpr Function<u64(u64 settings, u64 cutoff)> kGenericS22{"audio_generic_s22"};
    inline constexpr Function<u64(u64 settings, float attenuation)> kGenericS23{"audio_generic_s23"};
    inline constexpr Function<u64(u64 settings, float scale)> kGenericS24{"audio_generic_s24"};
    inline constexpr Function<u64(u64 entity, std::uint8_t create)> kEntityEnvGroup{"audio_entity_env_group"};
    inline constexpr Function<std::uint32_t*(std::uint32_t* bank, u64 soundRef, std::uint32_t* count)>
        kRequiredDynamicBank{"audio_required_dynamic_bank"};
    inline constexpr Function<u64(std::uint32_t bank)> kFindBankSlot{"audio_find_bank_slot"};
    inline constexpr Function<std::uint32_t(u64 slot, std::uint32_t bank)> kBankLoadStatus{"audio_bank_load_status"};
    inline constexpr Function<std::uint8_t*(void* ped)> kPedAudioGetter{"audio_ped_audio_getter"};
    inline constexpr Function<void*(std::uint32_t key)> kRefContainer{"audio_ref_container"};
    inline constexpr Function<u64(std::uint32_t hash, std::uint32_t variant)> kWeaponSettingsLookup{
        "audio_weapon_settings_lookup"};
    inline constexpr Function<void(void* params)> kSpeechParamsCtor{"audio_speech_params_ctor"};
    inline constexpr Function<void(void* params)> kSpeechParamsDtor{"audio_speech_params_dtor"};
    inline constexpr Function<void(void* scope)> kSpeechScopeCtor{"audio_speech_scope_ctor"};
    inline constexpr Function<void(void* scope)> kSpeechScopeRelease{"audio_speech_scope_release"};
    inline constexpr Function<void(void* scope)> kSpeechScopeDtor{"audio_speech_scope_dtor"};
    inline constexpr Function<std::uint32_t(void* speech, void* scope, void* params)> kSpeechDirectEntry{
        "speech_direct_entry"};
    inline constexpr Function<std::uint32_t(void* line)> kSpeechLineStart{"audio_speech_line_start"};
    inline constexpr Function<void(void* line, std::uint32_t reason)> kSpeechLineStop{"audio_speech_line_stop"};
    inline constexpr Function<std::uint32_t(void* speech, std::uint32_t context, void* params)> kSpeechAnimalVocal{
        "speech_animal_vocal"};
    inline constexpr Function<void*(void* store, std::uint32_t hash, std::uint8_t type)> kVoiceLookup{
        "audio_voice_lookup"};
    inline constexpr Function<void(void* holder, void* voice, std::uint8_t, std::uint8_t, void* callback)> kVoiceApply{
        "audio_voice_apply"};
    inline constexpr Function<void*()> kSpeechSetMetadata{"metadata_prologue"};
    inline constexpr Function<std::uint32_t*(void* metadata, std::uint32_t* out, std::uint32_t hash,
        std::int32_t typeFilter)>
        kSpeechSetRefByHash{"ref_by_hash_prologue"};
    inline constexpr Function<void*(u64 ref)> kSpeechSetObject{"object_prologue"};
    inline constexpr Function<void(void* melee, void* info)> kMeleeImpact{"audio_melee_impact"};
    inline constexpr Function<void*(void* manager, std::uint32_t hash)> kActionResultLookup{
        "audio_action_result_lookup"};
    inline constexpr Function<std::uint32_t*(std::uint32_t* out, std::int32_t tag)> kExplosionTagRef{
        "audio_explosion_tag_ref"};
    inline constexpr Function<void(void* manager, std::uint32_t ref, const float* position, std::uint8_t underwater,
        std::uint8_t, std::uint32_t interior, std::uint8_t)>
        kExplosionPlay{"audio_explosion_play"};
    inline constexpr Function<void(const void* info)> kFireEnqueue{"audio_fire_enqueue"};
    inline constexpr Function<void(void* footAudio, std::int32_t event, std::uint32_t, std::uint8_t foot, std::uint8_t,
        std::uint8_t flag, std::uint32_t, std::uint8_t)>
        kFootstep{"audio_footstep"};
    inline constexpr Function<void(void* footAudio, std::int32_t type, std::uint8_t foot, std::uint8_t, std::uint8_t)>
        kFootEventEnqueue{"foot_event_enqueue"};
    inline constexpr Function<void(void* audio, const void* event)> kAddAnimEvent{"audio_add_anim_event"};

    // burn_level.cpp: the char record copy, a volume's clone and link, the
    // removal of an entity's records.
    inline constexpr Function<void*(void* manager, void* source, void* entity, std::uint8_t copyVolumes)> kBurnRecordCopy{
        "burn_record_copy"};
    inline constexpr Function<void*(void* source, void* record, void* fire)> kBurnClone{"burn_clone"};
    inline constexpr Function<void(void* record, void* volume, std::uint8_t)> kBurnLink{"burn_link"};
    inline constexpr Function<void(void* manager, void* entity, std::int32_t, std::uint8_t)> kBurnRemove{"burn_remove"};

    // decal_observer.cpp: the add with an id (the core) and its wrapper, the
    // projected add, the removal, the alive test, the info lookup.
    inline constexpr Function<int(void* manager, void* entity, void* settings, int id, std::uint8_t flag)> kDecalCore{
        "decal_core"};
    inline constexpr Function<void*(void* manager, const std::uint32_t* type, void* entity, int, const float* position,
        const float* direction, const float* side, float size, std::uint16_t, std::uint8_t, int, int, void* owner)>
        kDecalProjAdd{"decal_proj_add"};
    inline constexpr Function<void(void* manager, int id)> kDecalRemove{"decal_remove"};
    inline constexpr Function<bool(void* manager, int id)> kDecalAlive{"decal_alive"};
    inline constexpr Function<void*(void* table, const std::uint32_t* hash, int index)> kDecalInfoLookup{
        "decal_info_lookup"};
    inline constexpr Function<int(void* manager, void* entity, void* settings, std::uint8_t flag)> kDecalWrapper{
        "decal_wrapper"};

    // entity_pools.cpp
    inline constexpr Function<std::int32_t(void* entity)> kCreateGuid{"pools_create_guid"};

    // fire_observer.cpp: a fire's start and stop, a remnant's removal, the
    // burnt ground's clear.
    inline constexpr Function<void*(void* manager, void* desc)> kFireStart{"fire_start"};
    inline constexpr Function<void(void* fire, std::uint8_t instant, std::uint8_t)> kFireStop{"fire_stop"};
    inline constexpr Function<void(void* manager, void* entry, std::uint8_t)> kFireRemnantRemove{"fire_remnant_remove"};
    inline constexpr Function<void(void* area, const float* center, const float* radius, std::uint8_t)> kFireAreaClear{
        "fire_area_clear"};

    // frag_observer.cpp
    inline constexpr Function<void(void* inst, std::uint16_t group)> kFragDeleteAboveGroup{"frag_delete_above_group"};
    inline constexpr Function<void*(void* inst, std::uint16_t group, void* data, bool flag)> kFragBreakOffAboveGroup{
        "frag_break_off_above_group"};

    // fx_observer.cpp: the particle native's start, the weapon and impact
    // paths' instance calls, the registered (owner, id) effects, the stop,
    // the parent lock.
    inline constexpr Function<bool(const std::uint32_t* effect, const std::uint32_t* asset, void* entity, int bone,
        const void* matrix, float scale, std::uint8_t flags)>
        kFxStart{"fx_start"};
    inline constexpr Function<std::uint8_t*(const std::uint32_t* effect)> kFxTriggeredInst{"fx_triggered_inst"};
    inline constexpr Function<void(void* inst, void* entity, int bone, int, std::uint8_t, std::uint8_t)> kFxAttach{
        "fx_attach"};
    inline constexpr Function<void(void* inst, const void* offset)> kFxSetOffsetPos{"fx_set_offset_pos"};
    inline constexpr Function<void(void* inst)> kFxInstStart{"fx_inst_start"};
    inline constexpr Function<void(void* inst, const void* matrix)> kFxSetMatrix{"fx_set_matrix"};
    inline constexpr Function<void*(void* inst, void* delta, const void* parent)> kFxSetParent{"fx_set_parent"};
    inline constexpr Function<void(void* inst, const void* list)> kFxAddPlanes{"fx_add_planes"};
    inline constexpr Function<void(void* inst, const void* aux)> kFxSetAux350{"fx_set_aux350"};
    inline constexpr Function<void*(void* owner, int id, void*, std::uint8_t flag, std::uint32_t* effect,
        std::uint32_t* asset, std::uint8_t* created, std::uint8_t, std::uint8_t)>
        kFxRegistered{"fx_registered"};
    inline constexpr Function<void(void* inst, std::uint8_t immediate)> kFxStop{"fx_stop"};
    inline constexpr Function<void*(void* manager, void* shooter, void* weapon, void* object, std::int32_t bone,
        const void* info, const void* vector, std::int32_t)>
        kFxMuzzleRequest{"fx_muzzle_request"};
    inline constexpr Function<void*(void* manager, void* shooter, void* weapon, void* object, std::int32_t,
        std::int32_t bone)>
        kFxShellRequest{"fx_shell_request"};
    inline constexpr Function<bool(void* lock)> kFxLockOwned{"fx_lock_owned"};
    inline constexpr Function<void(void* lock)> kFxLock{"fx_lock"};
    inline constexpr Function<void(void* lock)> kFxUnlock{"fx_unlock"};

    // game_listener.cpp: the metadata lookup by hash.
    inline constexpr Function<std::uintptr_t(std::uintptr_t table, std::uint32_t hash, std::uint8_t type)>
        kListenerMetadataLookup{"listener_metadata_lookup"};

    // mix_scene.cpp: the conversation duck's start and stop, the mixer's
    // start and stop, the temporary list's release, a scene variable, the
    // group add.
    inline constexpr Function<void(void* conversation)> kConversationDuckStart{"conversation_duck_start"};
    inline constexpr Function<void(void* conversation)> kConversationDuckStop{"mix_conversation_stop"};
    inline constexpr Function<std::uint8_t(void* mixer, const std::uint32_t* hash, std::uint64_t* reference, void* owner,
        std::uint32_t offset, void*, void* temp)>
        kMixerStartScene{"mixer_start_scene"};
    inline constexpr Function<void(void* temp)> kMixTempRelease{"mix_temp_release"};
    inline constexpr Function<std::uint64_t(std::uint64_t scene, std::uint64_t)> kMixerStopScene{"mixer_stop_scene"};
    inline constexpr Function<void(std::uint64_t scene, std::uint32_t name, float value)> kMixVarSet{"mix_var_set"};
    inline constexpr Function<void(void* mixer, void* entity, std::uint32_t, std::uint32_t hash, float fade)> kMixGroupAdd{
        "mix_group_add"};

    // ped_blood.cpp: the blood component (create, read), its slot, the add,
    // the clear, the dirt setters, the finalize.
    inline constexpr Function<void*(void* ped)> kBloodCompCreate{"blood_comp_create"};
    inline constexpr Function<void*(void* ped)> kBloodCompRead{"blood_comp_read"};
    inline constexpr Function<void(void* manager, void* comp)> kBloodSlotAlloc{"blood_slot_alloc"};
    inline constexpr Function<void*(void* manager, std::uint8_t slot, const void* buffer, std::uint8_t pack)> kBloodAdd{
        "blood_add"};
    inline constexpr Function<void(void* comp)> kBloodClear{"blood_clear"};
    inline constexpr Function<void(void* component, std::int32_t value)> kBloodDirtSet3A8{"blood_dirt_set_3a8"};
    inline constexpr Function<void(void* component, std::int32_t value)> kBloodDirtSet3AC{"blood_dirt_set_3ac"};
    inline constexpr Function<void(void* component, std::int32_t value)> kBloodDirtSet3B0{"blood_dirt_set_3b0"};
    inline constexpr Function<void(void* record, void* held, std::uint8_t)> kBloodFinalize{"blood_finalize"};

    // ped_gore.cpp: the limb's gore apply, the gore event handler.
    inline constexpr Function<bool(int limb, void* context, std::uint32_t* hash, std::uint8_t)> kGoreApply{"gore_apply"};
    inline constexpr Function<bool(void* event)> kGoreHandler{"gore_handler"};

    // rope_observer.cpp
    inline constexpr Function<void(std::uint64_t entry, std::uint64_t entity)> kRopeSetAttachmentEntity{
        "rope_set_attachment_entity"};

    // timecycle_state.cpp
    inline constexpr Function<void(void* weather, std::uint32_t type, std::int32_t variation, std::uint8_t immediate)>
        kTimecycleSetVariation{"timecycle_set_variation"};

    // world_theatre_engine.cpp: the world's clean-ups.
    inline constexpr Function<void(bool respectCanBeDeleted)> kRemoveAllVehs{"theatre_remove_all_vehs"};
    inline constexpr Function<void(void* exception, int mode, void* filter, bool asScript, bool cleanup)> kRemoveAllPeds{
        "theatre_remove_all_peds"};
    inline constexpr Function<void(void* manager)> kStopStaticEmitters{"theatre_stop_and_reset_static_emitters"};
    inline constexpr Function<void(void* manager, void* speechEntity)> kFreeAmbientSlots{
        "theatre_free_ambient_slots_of_owner"};

    // script.cpp: the camera by handle, the savegame queue push, the load
    // from a slot.
    inline constexpr Function<void*(int handle)> kCameraLookup{"camera_lookup"};
    inline constexpr Function<bool(void* operation)> kSavegamePush{"worldmodel_push"};
    inline constexpr Function<void(int slot)> kLoadFromSlot{"worldmodel_load_from_slot"};
}
