#include <intrin.h>
#include "mod_identity.h" // RedemptionEditor.ini (ReplayProbe.ini read too)
#include "guarded_memory.h"
#include "sound_census.h"
#include "horse_sound_report.h"
#include "sound_census_core.h"

#include "audio_observer.h"
#include "engine_hud.h" // the sound check's notice
#include "entity_cache.h"
#include "hook_batch.h"
#include "module_lifetime.h"
#include "event_ring.h"
#include "profiler.h"
#include "sound_generic.h"
#include "soundfx_probe.h"
#include "version.generated.h"

#include <windows.h>

#include <MinHook.h>

#include "main.h"
#include "natives.h"

#include <atomic>
#include <cstring>
#include <ctime>
#include <fstream>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace replay::census
{
    namespace
    {
        // The two sound instance creators (see sound_census_core.h). Both prologues
        // are checked, and the call to the slot allocator 0x251BAC4 inside each (the
        // fact that makes them the only creators).
        constexpr std::uintptr_t kCreatorARva = 0x2516CAC;
        constexpr std::uint8_t kCreatorASignature[] = {
            0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x4C, 0x89, 0x48, 0x20, 0x4C, 0x89, 0x40, 0x18, 0x55,
            0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xA9, 0x48, 0x81,
            0xEC, 0xF0, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0xF1, 0x4D, 0x8B, 0xF8};
        constexpr std::uintptr_t kCreatorAAllocCall = 0x2516D2A;
        constexpr std::uint8_t kCreatorAAllocCallBytes[] = {0xE8, 0x95, 0x4D, 0x00, 0x00};
        constexpr std::uintptr_t kCreatorBRva = 0x2516900;
        constexpr std::uint8_t kCreatorBSignature[] = {
            0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54,
            0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xE9, 0x48, 0x81, 0xEC, 0xE8, 0x00,
            0x00, 0x00, 0x48, 0x8D, 0x81, 0x28, 0xF8, 0x00, 0x00};
        constexpr std::uintptr_t kCreatorBAllocCall = 0x2516973;
        constexpr std::uint8_t kCreatorBAllocCallBytes[] = {0xE8, 0x4C, 0x51, 0x00, 0x00};
        // (manager, u64 ref, audEntity*, audSound** or null, init params or
        // null) -> audSound* or 0. Five arguments: both read their frame up
        // to entry rsp+0x28 only.
        using CreatorFn = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t);

        constexpr std::uint32_t kRing = 8192;
        using Slot = replay::ring::Slot<Row>; // event_ring.h

        // Ini (read once): -1 not read.
        int g_iniRead = 0;
        bool g_check = false;  // SoundCheck (default = Debug)
        bool g_full = false;   // SoundCensus (a line per sound)
        int g_capture = 1;     // SoundCapture
        // The on-screen notice of the report at the playback's stop, shown with
        // Debug=0 too: at least this many MISSING, FEWER, EXTRA and MORE findings
        // (SoundCheckNotice, default 3; 0: no notice).
        std::uint32_t g_noticeMin = replay::census::kNoticeMinFindings;
        std::vector<Finding> g_extraFindings; // NoteExtraFindings
        // The check's reports, in the game folder, apart from the player's clips.
        constexpr wchar_t kReportFolder[] = L"ReplayDebug";

        Slot* g_ring = nullptr;
        std::atomic<std::uint64_t> g_next{0};
        std::atomic<CreatorFn> g_originalA{nullptr};
        std::atomic<CreatorFn> g_originalB{nullptr};
        void* g_targetA = nullptr;
        void* g_targetB = nullptr;
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
        LARGE_INTEGER g_frequency{};
        // Detour cost (QPC ticks inside Push, all threads).
        std::atomic<std::uint64_t> g_pushTicks{0};
        std::atomic<std::uint64_t> g_pushCalls{0};
        thread_local int t_modDepth = 0; // ModScope

        // Script thread state.
        std::uint64_t g_cursor = 0;
        const char* g_phase = "idle";
        std::uint64_t g_lastTickUs = 0;
        double g_lastTickMs = 0.0;
        bool g_lastNormal = false;
        std::uint64_t g_ownersBuiltUs = 0;
        OwnerTable g_owners;
        std::uint32_t g_playerId = 0xFFFF0000u; // the player's recorded id (last recording)
        // The recorded player's lines are said by the hidden live player in this
        // playback (sound_check_core.h ProxySpeechKey).
        bool g_playerSpeechProxy = false;
        struct Totals
        {
            std::uint64_t rows = 0;
            std::uint64_t refused = 0;
            std::uint64_t unowned = 0;
            std::uint64_t dropped = 0;
            std::uint64_t coreB = 0;
            std::uint64_t player = 0;
            std::uint64_t counted = 0; // rows counted into the summary (normal time)
            std::uint64_t learned = 0; // audio entities learned through their tracker
        };
        Totals g_totals;
        std::uint64_t g_idleDropped = 0;
        std::uint64_t g_pushTicksAtPhase = 0;
        std::uint64_t g_pushCallsAtPhase = 0;

        // The cheap mode.
        Accumulator g_recordAcc;
        Accumulator g_playAcc;
        Summary g_playSummary;
        PlayCoverage g_playCover; // what the playhead did per window
        double g_lastCoverMs = -1.0;
        bool g_playing = false;
        std::vector<TickRow> g_rows;
        std::vector<LiveSound> g_live;
        std::uint64_t g_liveCheckedUs = 0;
        constexpr std::size_t kLiveMax = 1024;
        std::unordered_map<std::uint32_t, OwnerClass> g_classes; // recorded id -> class (this phase)
        struct Learned
        {
            Owner owner;          // the audio entity's range, base Field
        };
        std::vector<Learned> g_learned;
        constexpr std::size_t kLearnedMax = 256;
        // Playback: the Field bases the clip's captured sounds use, per id.
        std::vector<std::pair<std::uint32_t, std::uint16_t>> g_playFields;
        std::wstring (*g_clipPathSource)() = nullptr;
        Cost g_cost;

        std::uint64_t QpcUs()
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            return static_cast<std::uint64_t>(now.QuadPart) * 1000000ull /
                static_cast<std::uint64_t>(g_frequency.QuadPart);
        }

        std::uint32_t ToRva(void* address)
        {
            const auto value = reinterpret_cast<std::uintptr_t>(address);
            return value >= g_base && value < g_imageEnd ? static_cast<std::uint32_t>(value - g_base) : 0;
        }

        bool GuardedCopy(void* out, const void* source, std::size_t size)
        {
            // Same rule as the audio observer: pointers below 4 GB or non-canonical are
            // refused without a fault (guarded_memory.h).
            return replay::mem::Copy(out, source, size, replay::mem::kGameHeapLowest);
        }

        bool GuardedCompare(const void* target, const std::uint8_t* signature, std::size_t size)
        {
            return replay::mem::Matches(target, signature, size);
        }

        void Push(std::uint8_t core, std::uint64_t ref, std::uint64_t audio, std::uint64_t out, std::uint64_t params,
            std::uint64_t sound)
        {
            // The sound of a mode c request through the game's queue goes to its ticket
            // (one relaxed load while none waits).
            if (sound && t_modDepth == 0)
                replay::audio::NoteQueueBirth(audio, ref, sound, out);
            Slot* ring = g_ring;
            if (!ring)
                return;
            LARGE_INTEGER start{};
            QueryPerformanceCounter(&start);
            Row row;
            row.timeUs = static_cast<std::uint64_t>(start.QuadPart) * 1000000ull /
                static_cast<std::uint64_t>(g_frequency.QuadPart);
            row.core = core;
            row.ref = ref;
            row.audio = audio;
            row.sound = sound;
            row.out = out;
            row.mine = t_modDepth > 0;
            if (params && GuardedCopy(row.params, reinterpret_cast<const void*>(params), sizeof(row.params)))
                ReadParams(row, row.params);
            // Frame 0 is the census detour, then the creator's caller (the entry
            // wrapper), the audEntity method, the game code that asked for the sound and
            // two above (kRowCallers frames).
            void* frames[kRowCallers] = {};
            const USHORT captured = RtlCaptureStackBackTrace(1, kRowCallers, frames, nullptr);
            for (USHORT i = 0; i < captured && i < kRowCallers; ++i)
                row.callers[i] = ToRva(frames[i]);
            // The read probe's tap on both creators (SoundFxProbe=1; off, one relaxed load).
            if (replay::soundfx::Active())
                replay::soundfx::NoteCreated(core, ref, audio, sound, row.hasParams ? row.params : nullptr, row.callers,
                    kRowCallers);
            // The generic sound recorder's create (one relaxed load while no recording runs).
            if (replay::soundgeneric::Capturing())
                replay::soundgeneric::OnCreated(core, ref, audio, sound, row.hasParams ? row.params : nullptr, row.callers,
                    kRowCallers, row.mine, replay::soundfx::InsideSpeech());
            {
                replay::ring::Writer<Row> slot(ring, kRing, g_next);
                slot.value = row;
                slot.Publish();
            }
            LARGE_INTEGER end{};
            QueryPerformanceCounter(&end);
            g_pushTicks.fetch_add(static_cast<std::uint64_t>(end.QuadPart - start.QuadPart), std::memory_order_relaxed);
            g_pushCalls.fetch_add(1, std::memory_order_relaxed);
        }

        std::uint64_t __fastcall CreatorADetour(std::uint64_t manager, std::uint64_t ref, std::uint64_t audio,
            std::uint64_t out, std::uint64_t params)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first (audit C01)
            REPLAY_PROFILE_ZONE("detour.sound_census");
            // A puppet vehicle's own create the recording plays is refused as the creator
            // refuses with its slot pool full (0x2516D42: 0 returned, the out slot
            // untouched; vehicle_sound_rule.h). The mod's own creates never ask.
            if (t_modDepth == 0 && replay::audio::RefuseVehicleOwnCreate(0, audio, ref, out, ToRva(_ReturnAddress())))
                return 0;
            const CreatorFn original = g_originalA.load(std::memory_order_acquire);
            const std::uint64_t sound = original ? original(manager, ref, audio, out, params) : 0;
            Push(0, ref, audio, out, params, sound);
            return sound;
        }

        std::uint64_t __fastcall CreatorBDetour(std::uint64_t manager, std::uint64_t ref, std::uint64_t audio,
            std::uint64_t out, std::uint64_t params)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first (audit C01)
            REPLAY_PROFILE_ZONE("detour.sound_census");
            // As creator A (0x251698B: the slot pool's refusal).
            if (t_modDepth == 0 && replay::audio::RefuseVehicleOwnCreate(1, audio, ref, out, ToRva(_ReturnAddress())))
                return 0;
            const CreatorFn original = g_originalB.load(std::memory_order_acquire);
            const std::uint64_t sound = original ? original(manager, ref, audio, out, params) : 0;
            Push(1, ref, audio, out, params, sound);
            return sound;
        }

        bool HookOne(std::uint8_t* base, std::uintptr_t rva, const std::uint8_t* signature, std::size_t size,
            std::uintptr_t allocCall, const std::uint8_t* allocBytes, void* detour, std::atomic<CreatorFn>& original,
            void*& target, const char* name, std::string& error)
        {
            void* address = base + rva;
            if (!GuardedCompare(address, signature, size) || !GuardedCompare(base + allocCall, allocBytes, 5))
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
            original.store(reinterpret_cast<CreatorFn>(trampoline), std::memory_order_release);
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

        std::uint64_t Address(Entity handle)
        {
            if (!handle || !replay::entities::Exists(handle))
                return 0;
            return reinterpret_cast<std::uint64_t>(replay::entities::AddressOf(handle));
        }

        bool HeapPointer(std::uint64_t value)
        {
            return replay::mem::InUserRange(static_cast<std::uintptr_t>(value), 8, replay::mem::kGameHeapLowest) &&
                (value & 7u) == 0;
        }

        // A vehicle's audio entity, [vehicle + 0x900] (sound_check_core.h).
        std::uint64_t VehicleAudioOf(std::uint64_t address)
        {
            std::uint64_t audio = 0;
            if (!address || !GuardedCopy(&audio, reinterpret_cast<const void*>(address + kVehicleAudioField), 8) ||
                !HeapPointer(audio))
                return 0;
            return audio;
        }

        enum class EntityShape : std::uint8_t
        {
            Ped,
            Vehicle,
            Object,
        };

        // Entity spans: a ped's audio holder and every audio entity in it
        // (kPedAudioSpan), its sound tracker (the component +0x10 the engine's tracker
        // getter returns) and the entity itself (objects use entity + offset as
        // tracker, the lit molotov +0x450); a weapon's audio (CWeapon +0x100 through
        // object +0x448); a vehicle's or train car's audio entity ([+0x900]) and the
        // Field bases learned or used by the clip's captured sounds.
        void AddEntity(Entity handle, EntityShape shape, OwnerKind kind, std::uint32_t id, OwnerClass cls,
            bool player)
        {
            const std::uint64_t address = Address(handle);
            if (!address)
                return;
            Owner owner;
            owner.kind = kind;
            owner.id = id;
            owner.player = player;
            owner.cls = cls;
            owner.entity = address;
            owner.lo = address;
            owner.hi = address + 0x1000;
            owner.base = AudioBase::Entity;
            g_owners.tracker.push_back(owner);
            g_owners.audio.push_back(owner);
            void* entity = reinterpret_cast<void*>(address);
            if (shape == EntityShape::Vehicle)
            {
                // [car+0x11E0] is the consist's engine car, not an audio entity (the whistle
                // native 0x10F7A24 passes it to 0x154D500, which reads its +0x900 audio).
                if (const std::uint64_t audio = VehicleAudioOf(address))
                {
                    Owner audioOwner = owner;
                    audioOwner.lo = audio;
                    audioOwner.hi = audio + kVehicleAudioSpan;
                    audioOwner.base = AudioBase::VehicleAudio;
                    g_owners.audio.push_back(audioOwner);
                    g_owners.tracker.push_back(audioOwner);
                }
            }
            else if (shape == EntityShape::Ped)
            {
                if (const std::uint64_t holder = replay::audio::PedAudioHolder(entity))
                {
                    Owner holderOwner = owner;
                    holderOwner.lo = holder;
                    holderOwner.hi = holder + replay::audio::kPedAudioSpan;
                    holderOwner.base = AudioBase::PedHolder;
                    g_owners.audio.push_back(holderOwner);
                }
                // The speech entity. The ped audio is one allocation (build 1491.50), 0x4A70
                // bytes for an animal (0x89DAC8) and 0x4CB0 for a person (0x89E4F8): the
                // component at +0, the holder at +0x250 and the speech entity at +0x48C0 =
                // holder + 0x4670 ([holder + 0x4438], vtable 0x37FE430 animal, 0x37FD948
                // person). The holder span (0x4820) ends at the animal's end, so a person's
                // speech entity lies 0x240 bytes past it. That tail is the ped's too (base
                // Entity with lo away from the entity: counted, never captured).
                if (const std::uint64_t holder = replay::audio::PedAudioHolder(entity))
                {
                    const std::uint64_t speech = replay::audio::SpeechEntity(entity);
                    std::uint64_t vtable = 0;
                    if (speech && GuardedCopy(&vtable, reinterpret_cast<const void*>(speech), 8))
                    {
                        const std::uint64_t image = g_base;
                        const std::uint64_t size = vtable == image + 0x37FD948 ? 0x3F0 : vtable == image + 0x37FE430 ? 0x1B0 : 0;
                        const std::uint64_t end = speech + size;
                        const std::uint64_t holderEnd = holder + replay::audio::kPedAudioSpan;
                        if (size && end > holderEnd)
                        {
                            Owner speechOwner = owner;
                            speechOwner.lo = speech > holderEnd ? speech : holderEnd;
                            speechOwner.hi = end;
                            speechOwner.base = AudioBase::Entity;
                            g_owners.audio.push_back(speechOwner);
                        }
                    }
                }
                if (const std::uint64_t tracker = replay::audio::PedSoundTracker(entity))
                {
                    Owner trackerOwner = owner;
                    trackerOwner.lo = tracker;
                    trackerOwner.hi = tracker + 0x10;
                    g_owners.tracker.push_back(trackerOwner);
                }
            }
            else if (const std::uint64_t weapon = replay::audio::WeaponAudio(entity))
            {
                Owner weaponOwner = owner;
                weaponOwner.lo = weapon;
                // The audio is at CWeapon + 0x100, so the range ends at + 0x200 as the
                // recorder's (SND kind 4).
                weaponOwner.hi = weapon + 0x200;
                weaponOwner.base = AudioBase::WeaponAudio;
                g_owners.audio.push_back(weaponOwner);
            }
            if (kind == OwnerKind::Puppet)
            {
                for (const auto& field : g_playFields)
                {
                    if (field.first != id)
                        continue;
                    std::uint64_t pointer = 0;
                    if (!GuardedCopy(&pointer, reinterpret_cast<const void*>(address + field.second), 8) ||
                        !HeapPointer(pointer))
                        continue;
                    Owner fieldOwner = owner;
                    fieldOwner.lo = pointer;
                    fieldOwner.hi = pointer + kFieldSpan;
                    fieldOwner.base = AudioBase::Field;
                    fieldOwner.field = field.second;
                    g_owners.audio.push_back(fieldOwner);
                }
            }
        }

        OwnerClass PedClass(std::uint32_t id, Entity handle, bool horse, bool player)
        {
            if (player)
                return OwnerClass::Player;
            if (horse)
                return OwnerClass::Horse;
            const auto found = g_classes.find(id);
            if (found != g_classes.end())
                return found->second;
            const OwnerClass cls = PED::IS_PED_HUMAN(handle) ? OwnerClass::Ped : OwnerClass::Animal;
            g_classes[id] = cls;
            return cls;
        }

        void BuildOwners(Context& context, bool playback)
        {
            REPLAY_PROFILE_ZONE("census.owners");
            g_owners.Clear();
            const Ped player = PLAYER::PLAYER_PED_ID();
            if (!playback)
            {
                for (const replay::recorder::RecordedEntity& entity : context.recorder.entities)
                {
                    const bool isPlayer = entity.handle == player;
                    if (isPlayer)
                        g_playerId = entity.id.Packed();
                    if (entity.vehicle)
                    {
                        AddEntity(entity.handle, EntityShape::Vehicle, OwnerKind::Recorded, entity.id.Packed(),
                            OwnerClass::Vehicle, false);
                        continue;
                    }
                    // The class once per id and phase (the create packet
                    // says horse; IS_PED_HUMAN tells a person from an animal).
                    bool horse = false;
                    const auto cached = g_classes.find(entity.id.Packed());
                    if (cached != g_classes.end())
                        horse = cached->second == OwnerClass::Horse;
                    else if (const replay::entity::CreatePacket* packet = context.clip.FindCreate(entity.id.Packed()))
                    {
                        horse = packet->kind == replay::entity::EntityKind::Horse;
                        if (horse)
                            g_classes[entity.id.Packed()] = OwnerClass::Horse;
                    }
                    AddEntity(entity.handle, EntityShape::Ped, OwnerKind::Recorded, entity.id.Packed(),
                        PedClass(entity.id.Packed(), entity.handle, horse, isPlayer), isPlayer);
                }
                for (const replay::recorder::RecordedWeapon& weapon : context.recorder.weapons)
                    AddEntity(weapon.object, EntityShape::Object, OwnerKind::Recorded, weapon.id.Packed(),
                        OwnerClass::Weapon, false);
                for (const replay::recorder::RecordedProp& prop : context.recorder.props)
                    AddEntity(prop.object, EntityShape::Object, OwnerKind::Recorded, prop.id.Packed(),
                        OwnerClass::Object, false);
                for (const replay::recorder::RecordedTrainCar& car : context.recorder.trainCars)
                    AddEntity(car.handle, EntityShape::Vehicle, OwnerKind::Recorded, car.id.Packed(),
                        OwnerClass::Train, false);
            }
            else
            {
                for (const replay::playback::Puppet& puppet : context.puppets.list)
                {
                    const std::uint32_t id = puppet.packet.id.Packed();
                    const bool isPlayer = id == g_playerId;
                    switch (puppet.packet.kind)
                    {
                    case replay::entity::EntityKind::Ped:
                    case replay::entity::EntityKind::Horse:
                        if (puppet.clone && replay::entities::Exists(puppet.clone))
                            AddEntity(puppet.clone, EntityShape::Ped, OwnerKind::Puppet, id,
                                PedClass(id, puppet.clone, puppet.packet.kind == replay::entity::EntityKind::Horse, isPlayer),
                                isPlayer);
                        break;
                    case replay::entity::EntityKind::Vehicle:
                        AddEntity(puppet.clone, EntityShape::Vehicle, OwnerKind::Puppet, id, OwnerClass::Vehicle, false);
                        break;
                    case replay::entity::EntityKind::TrainCar:
                        AddEntity(puppet.clone, EntityShape::Vehicle, OwnerKind::Puppet, id, OwnerClass::Train, false);
                        break;
                    case replay::entity::EntityKind::Weapon:
                        AddEntity(puppet.clone, EntityShape::Object, OwnerKind::Puppet, id, OwnerClass::Weapon, false);
                        break;
                    default:
                        AddEntity(puppet.clone, EntityShape::Object, OwnerKind::Puppet, id, OwnerClass::Object, false);
                        break;
                    }
                }
                AddEntity(player, EntityShape::Ped, OwnerKind::LivePlayer, 0xFFFF0000u, OwnerClass::LivePlayer, true);
            }
            // Learned audio entities whose owner is still where it was.
            g_learned.erase(std::remove_if(g_learned.begin(), g_learned.end(), [&](const Learned& learned) {
                for (const Owner& owner : g_owners.tracker)
                    if (owner.id == learned.owner.id && owner.kind == learned.owner.kind && owner.entity == learned.owner.entity)
                        return false;
                return true; }), g_learned.end());
            for (const Learned& learned : g_learned)
                g_owners.audio.push_back(learned.owner);
        }

        // A row owned through its tracker whose audio entity is in no range: where
        // that audio entity hangs off the owner (one level, FindFieldBase), so its rows
        // without a tracker find the owner too.
        void Learn(const Row& row, const Owner& owner)
        {
            if (g_learned.size() >= kLearnedMax || !HeapPointer(row.audio) || !owner.entity ||
                (row.audio >= g_base && row.audio < g_imageEnd))
                return;
            if (OwnerTable::In(g_owners.audio, row.audio))
                return;
            std::uint8_t bytes[kFieldScanBytes];
            if (!GuardedCopy(bytes, reinterpret_cast<const void*>(owner.entity), sizeof(bytes)))
                return;
            std::uint16_t field = 0;
            std::int32_t offset = 0;
            if (!FindFieldBase(bytes, sizeof(bytes), row.audio, field, offset))
                return;
            Learned learned;
            learned.owner = owner;
            learned.owner.lo = row.audio - static_cast<std::uint64_t>(offset);
            learned.owner.hi = row.audio + 0x100;
            learned.owner.base = AudioBase::Field;
            learned.owner.field = field;
            g_learned.push_back(learned);
            g_owners.audio.push_back(learned.owner);
            ++g_totals.learned;
        }

        void CloseTotals(std::ostream& log, const char* phase)
        {
            const std::uint64_t pushCalls = g_pushCalls.load(std::memory_order_relaxed) - g_pushCallsAtPhase;
            const std::uint64_t pushTicks = g_pushTicks.load(std::memory_order_relaxed) - g_pushTicksAtPhase;
            const double pushUs = pushCalls && g_frequency.QuadPart
                ? static_cast<double>(pushTicks) * 1e6 / static_cast<double>(g_frequency.QuadPart) / static_cast<double>(pushCalls)
                : 0.0;
            log << "SOUND_CENSUS_TOTAL,ph=" << phase << ",rows=" << g_totals.rows << ",refused=" << g_totals.refused
                << ",unowned=" << g_totals.unowned << ",player=" << g_totals.player << ",core_b=" << g_totals.coreB
                << ",dropped=" << g_totals.dropped << ",idle_dropped=" << g_idleDropped
                << ",counted=" << g_totals.counted << ",learned=" << g_totals.learned
                << ",mode=" << (g_full ? "full" : "cheap") << ",tick_us_avg="
                << (g_cost.ticks ? g_cost.tickUsTotal / static_cast<double>(g_cost.ticks) : 0.0)
                << ",tick_us_max=" << g_cost.tickUsMax << ",push_us_avg=" << pushUs << ",live=" << g_live.size() << '\n';
            g_totals = Totals{};
            g_idleDropped = 0;
            g_cost = Cost{};
            g_pushCallsAtPhase = g_pushCalls.load(std::memory_order_relaxed);
            g_pushTicksAtPhase = g_pushTicks.load(std::memory_order_relaxed);
        }

        // Rows since the cursor; `dropped` counts rows the ring overwrote
        // or lost. A row not yet published is read on the next drain.
        template <typename Fn>
        void Drain(Fn&& each, std::uint64_t& dropped)
        {
            const replay::ring::Drained drained = replay::ring::ForEachSince(g_ring, kRing, g_next, g_cursor, each);
            dropped += drained.overflowed + drained.lost;
        }

        // The live persistent sounds (any phase).
        bool NoteLive(const Row& row)
        {
            if (!row.sound || !row.out || !HeapPointer(row.out))
                return false;
            std::uint64_t current = 0;
            if (!GuardedCopy(&current, reinterpret_cast<const void*>(row.out), 8) || current != row.sound)
                return false;
            for (LiveSound& live : g_live)
            {
                if (live.slot == row.out)
                {
                    live.sound = row.sound;
                    live.row = row;
                    return true;
                }
            }
            if (g_live.size() >= kLiveMax)
                g_live.erase(g_live.begin());
            LiveSound live;
            live.sound = row.sound;
            live.slot = row.out;
            live.row = row;
            g_live.push_back(live);
            return true;
        }

        void RefreshLive(std::uint64_t now)
        {
            if (now - g_liveCheckedUs < 250000)
                return;
            g_liveCheckedUs = now;
            g_live.erase(std::remove_if(g_live.begin(), g_live.end(), [](const LiveSound& live) {
                std::uint64_t current = 0;
                return !GuardedCopy(&current, reinterpret_cast<const void*>(live.slot), 8) || current != live.sound;
            }), g_live.end());
        }

        std::string Narrow(const std::wstring& text)
        {
            std::string out;
            for (const wchar_t c : text)
                out += c < 128 ? static_cast<char>(c) : '?';
            return out;
        }

        IgnoreList LoadIgnore()
        {
            IgnoreList list = DefaultIgnoreList();
#if defined(REDEMPTION_DEV)
            // The sound check is a developer tool: ReplayDebug\sound-ignore.txt adds refs to
            // ignore; the older file names are still read.
            std::ifstream file(replay::storage::Find(L"ReplayDebug\\sound-ignore.txt").c_str(), std::ios::binary);
            if (!file)
                file.open(replay::storage::Find(L"ReplayDebug\\som-ignorar.txt").c_str(), std::ios::binary);
            if (!file)
                file.open(replay::storage::Find(L"ReplayClips\\som-ignorar.txt").c_str(), std::ios::binary);
            if (file)
            {
                std::stringstream text;
                text << file.rdbuf();
                list.AddText(text.str());
            }
#endif
            return list;
        }
    }

    static void ReadIni()
    {
        if (g_iniRead)
            return;
        g_iniRead = 1;
        char directory[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, directory);
        const std::string ini = replay::identity::IniPathIn(directory);
        g_full = replay::identity::DevIniInt("SoundCensus", 0, ini.c_str()) != 0;
        // The check and its report are the bench's (Debug=1) unless SoundCheck is set
        // explicitly; the product keeps the hooks only for SoundCapture.
        const int check = static_cast<int>(replay::identity::DevIniInt("SoundCheck", -1, ini.c_str()));
        g_check = check < 0 ? replay::identity::DevIniInt("Debug", 0, ini.c_str()) != 0 : check != 0;
        g_capture = static_cast<int>(replay::identity::DevIniInt("SoundCapture", 1, ini.c_str()));
        const int notice = static_cast<int>(replay::identity::DevIniInt("SoundCheckNotice",
            static_cast<int>(kNoticeMinFindings), ini.c_str()));
        g_noticeMin = notice < 0 ? 0u : static_cast<std::uint32_t>(notice);
        if (g_capture < 0 || g_capture > 2)
            g_capture = 1;
    }

    bool Enabled()
    {
        ReadIni();
        return g_check || g_full || g_capture > 0; // the capture needs the hooks on its own
    }

    bool CheckEnabled()
    {
        ReadIni();
        return g_check;
    }

    bool FullLog()
    {
        ReadIni();
        return g_full;
    }

    int CaptureMode()
    {
        ReadIni();
        return Enabled() ? g_capture : 0;
    }

    bool Install(std::string& error)
    {
        if (g_targetA || g_targetB)
            return true;
        if (!Enabled())
        {
            error = "off";
            return false;
        }
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
        if (!g_ring)
            g_ring = new Slot[kRing];
        // The tickets start again at 0: the slots too (a slot holding an older
        // install's ticket would refuse the new ones, event_ring.h Claim). No
        // hook of the census is in place here.
        for (std::uint32_t n = 0; n < kRing; ++n)
        {
            g_ring[n].sequence.store(0, std::memory_order_relaxed);
            g_ring[n].dropped.store(0, std::memory_order_relaxed);
        }
        g_next.store(0, std::memory_order_release);
        g_cursor = 0;
        HookOne(base, kCreatorARva, kCreatorASignature, sizeof(kCreatorASignature), kCreatorAAllocCall,
            kCreatorAAllocCallBytes, reinterpret_cast<void*>(&CreatorADetour), g_originalA, g_targetA, "creator_a", error);
        HookOne(base, kCreatorBRva, kCreatorBSignature, sizeof(kCreatorBSignature), kCreatorBAllocCall,
            kCreatorBAllocCallBytes, reinterpret_cast<void*>(&CreatorBDetour), g_originalB, g_targetB, "creator_b", error);
        // Both or nothing: with one creator only the census would report the
        // other's sounds as missing on both sides and prove nothing.
        if (!g_targetA || !g_targetB)
        {
            for (void** target : {&g_targetA, &g_targetB})
            {
                if (*target)
                {
                    MH_DisableHook(*target);
                    MH_RemoveHook(*target);
                    *target = nullptr;
                }
            }
            g_originalA.store(nullptr, std::memory_order_release);
            g_originalB.store(nullptr, std::memory_order_release);
            return false;
        }
        return true;
    }

    void Tick(Context& context, const char* phase, double clipMs, bool normal)
    {
        if (!g_ring || !g_targetA)
            return;
        REPLAY_PROFILE_ZONE("census.tick");
        const std::uint64_t now = QpcUs();
        std::ostream& log = context.log;
        const bool playback = std::strcmp(phase, "play") == 0;
        g_rows.clear();
        // A new phase opens with SOUND_CENSUS_PHASE; rows made before its
        // first tick take that tick's clip time. A tick after a stall (half a
        // second without one, e.g. a paused playback that skips its events)
        // re-anchors the clock the same way.
        const bool newPhase = std::strcmp(phase, g_phase) != 0;
        if (newPhase)
        {
            if (std::strcmp(g_phase, "idle") != 0)
                CloseTotals(log, g_phase);
            g_phase = phase;
            g_ownersBuiltUs = 0;
            g_totals = Totals{};
            g_classes.clear();
            g_learned.clear();
            g_lastNormal = false;
            // The recording counts into its clip, a playback into its own summary
            // (BeginPlayback).
            if (playback)
                g_playAcc.Attach(&g_playSummary, true);
            else
                g_recordAcc.Attach(&context.clip.soundCensus, false);
        }
        Accumulator& acc = playback ? g_playAcc : g_recordAcc;
        if (!playback && acc.Target() != &context.clip.soundCensus)
            acc.Attach(&context.clip.soundCensus, false);
        const bool stalled = now - g_lastTickUs > 500000;
        if (newPhase || stalled)
        {
            g_lastTickUs = now;
            g_lastTickMs = clipMs;
            acc.Break();
        }
        if (now - g_ownersBuiltUs > 250000)
        {
            BuildOwners(context, playback);
            g_ownersBuiltUs = now;
        }
        if (newPhase)
            log << "SOUND_CENSUS_PHASE,ph=" << phase << ",t=" << clipMs << ",us=" << now
                << ",player_id=" << PackedIdText(g_playerId) << ",audio_owners=" << g_owners.audio.size()
                << ",tracker_owners=" << g_owners.tracker.size() << ",idle_dropped=" << g_idleDropped
                << ",mode=" << (g_full ? "full" : "cheap") << '\n';
        // The windows the clock ran through in normal time (only for the check; the
        // capture reads the rows below).
        if (g_check)
        {
            if (normal && g_lastNormal)
                acc.Cover(g_lastTickMs, clipMs);
            else if (normal)
                acc.Cover(clipMs, clipMs);
            else
                acc.Break();
            // The same windows with how the playhead went through them (rate), and the
            // ones it only stood on or passed scrubbing, for the report's coverage block.
            if (playback)
            {
                const double from = g_lastCoverMs >= 0.0 ? g_lastCoverMs : clipMs;
                if (normal)
                {
                    const double rate = context.transport.Rate();
                    g_playCover.Note(g_lastNormal ? g_lastTickMs : clipMs, clipMs, RateFlag(rate), rate);
                }
                else if (context.transport.Paused())
                    g_playCover.Note(clipMs, clipMs, kCoverPaused);
                else
                    g_playCover.Note(from, clipMs, kCoverScrub);
                g_lastCoverMs = clipMs;
            }
        }
        g_lastNormal = normal;
        Drain([&](const Row& row) {
            TickRow tick;
            tick.row = row;
            tick.clipMs = ClipMsAt(g_lastTickUs, g_lastTickMs, now, clipMs, row.timeUs);
            tick.normal = normal;
            Match match = Match::None;
            const Owner* owner = g_owners.Find(row.audio, row.tracker, match);
            if (owner && match == Match::Tracker && !playback)
                Learn(row, *owner);
            if (owner)
                tick.owner = *owner;
            tick.alive = NoteLive(row);
            ++g_totals.rows;
            g_totals.refused += row.sound ? 0 : 1;
            g_totals.unowned += owner ? 0 : 1;
            g_totals.coreB += row.core ? 1 : 0;
            g_totals.player += owner && owner->player ? 1 : 0;
            // Everything the replay made counts, the capture's own included
            // (the comparison is of totals per owner and ref).
            if (normal && g_check)
            {
                SummaryKey key;
                key.owner = owner ? owner->id : kNoOwner;
                key.cls = owner ? owner->cls : OwnerClass::World;
                key.ref = row.ref;
                for (std::uint32_t n = 0; n < kPathDepth; ++n)
                    key.path[n] = row.callers[n + 1];
                ProxySpeechKey(playback && g_playerSpeechProxy, g_playerId, key);
                acc.Add(tick.clipMs, key, row.sound == 0);
                ++g_totals.counted;
            }
            if (g_full)
            {
                LineContext line;
                line.phase = phase;
                line.clipMs = tick.clipMs;
                line.normal = normal;
                line.owner = owner;
                line.match = match;
                log << FormatRow(row, line) << '\n';
            }
            g_rows.push_back(tick);
        }, g_totals.dropped);
        RefreshLive(now);
        g_lastTickUs = now;
        g_lastTickMs = clipMs;
        const double us = static_cast<double>(QpcUs() - now);
        ++g_cost.ticks;
        g_cost.tickUsTotal += us;
        g_cost.tickUsMax = (std::max)(g_cost.tickUsMax, us);
        g_cost.rows += g_rows.size();
    }

    void Idle(Context& context)
    {
        if (!g_ring || !g_targetA)
            return;
        const std::uint64_t now = QpcUs();
        if (now - g_lastTickUs < 300000)
            return;
        REPLAY_PROFILE_ZONE("census.idle");
        if (std::strcmp(g_phase, "idle") != 0)
        {
            CloseTotals(context.log, g_phase);
            g_phase = "idle";
        }
        g_rows.clear();
        std::uint64_t ignored = 0;
        Drain([&](const Row& row) {
            ++g_idleDropped;
            NoteLive(row); // a loop made before a recording starts
        }, ignored);
        RefreshLive(now);
    }

    void Shutdown()
    {
        for (void** target : {&g_targetA, &g_targetB})
        {
            if (*target)
            {
                MH_DisableHook(*target);
                MH_RemoveHook(*target);
                *target = nullptr;
            }
        }
        g_originalA.store(nullptr, std::memory_order_release);
        g_originalB.store(nullptr, std::memory_order_release);
        // The unload drained every detour before this (module_lifetime.h),
        // so no Push runs on the ring any more.
        delete[] g_ring;
        g_ring = nullptr;
    }

    void BeginPlayback(Context& context)
    {
        g_playSummary.Clear();
        g_playAcc.Attach(&g_playSummary, true);
        g_playCover.Clear();
        g_lastCoverMs = -1.0;
        g_playing = true;
        // The player's recorded id from the clip's census (a clip from
        // another session has another one than the last recording).
        for (const SummaryKey& key : context.clip.soundCensus.keys)
        {
            if (key.cls == OwnerClass::Player)
            {
                g_playerId = key.owner;
                break;
            }
        }
        g_playFields.clear();
        for (const GeneralSound& sound : context.clip.generalSounds)
        {
            if (sound.base != AudioBase::Field)
                continue;
            const std::pair<std::uint32_t, std::uint16_t> key{sound.entity, sound.field};
            if (std::find(g_playFields.begin(), g_playFields.end(), key) == g_playFields.end())
                g_playFields.push_back(key);
        }
        g_ownersBuiltUs = 0;
    }

    void ForgetPlaybackAfterRestart()
    {
        g_playing = false;
        g_playAcc.Attach(nullptr, false);
    }

    void EndPlayback(Context& context, const std::string& captureLine, const std::string& summary,
        const std::string& section)
    {
        if (!g_playing)
            return;
        g_playing = false;
        if (!g_ring || !g_targetA || !g_check)
            return;
        REPLAY_PROFILE_ZONE("census.report");
        const std::uint64_t started = QpcUs();
        const DiffResult diff = Diff(context.clip.soundCensus, g_playSummary, g_playAcc.Passes(), LoadIgnore());
        std::wstring clipPath = g_clipPathSource ? g_clipPathSource() : std::wstring();
        std::wstring reportPath;
        std::string clipName = "no-file";
        if (!clipPath.empty())
        {
            const std::size_t slash = clipPath.find_last_of(L"\\/");
            const std::size_t dot = clipPath.find_last_of(L'.');
            const std::wstring stem = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)
                ? clipPath.substr(0, dot) : clipPath;
            // In ReplayDebug, not next to the player's clips.
            const std::wstring stemName = stem.substr(slash == std::wstring::npos ? 0 : slash + 1);
            reportPath = replay::storage::Path(kReportFolder) + L"\\" + stemName + L".sound-check.txt";
            clipName = Narrow(clipPath.substr(slash == std::wstring::npos ? 0 : slash + 1));
        }
        else
            reportPath = replay::storage::Path(kReportFolder) + L"\\no-file.sound-check.txt";
        bool written = false;
        if (diff.totals.coveredMs > 0 || !diff.totals.recordedCensus)
        {
            ReportHeader header;
            header.clip = clipName;
            header.version = replay::build::kVersion;
            header.durationMs = diff.totals.recordedMs;
            header.capture = captureLine;
            if (diff.totals.recordedCensus)
                header.coverage = FormatCoverage(context.clip.soundCensus, g_playCover, g_playAcc.Passes());
            std::time_t t = std::time(nullptr);
            std::tm local{};
            localtime_s(&local, &t);
            char when[32]{};
            std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
            header.when = when;
            replay::storage::Folder(kReportFolder); // the base folder (storage_paths.h)
            std::ofstream file(reportPath, std::ios::binary | std::ios::trunc);
            if (file)
            {
                file << replay::horsesound::InsertIntoReport(FormatReport(diff, header), summary, section);
                written = static_cast<bool>(file);
            }
        }
        context.log << FormatSummaryLine(diff, clipName, written ? Narrow(reportPath) : std::string("none"))
                    << ",report_us=" << (QpcUs() - started) << '\n';
        // The report's finding on screen for a few seconds (the engine's objective
        // line), Debug=0 too.
        if (written)
        {
            // The generic player's sounds that fell short count as missing.
            DiffResult noticeDiff = diff;
            for (Finding& extra : g_extraFindings)
                noticeDiff.findings.push_back(std::move(extra));
            g_extraFindings.clear();
            const std::string notice = FormatNotice(noticeDiff, g_noticeMin);
            if (!notice.empty())
            {
                replay::hud::ShowNotice(notice.c_str());
                context.log << "SOUND_CHECK_NOTICE_SUMMARY,text=" << notice << '\n';
            }
        }
    }

    void SetClipPathSource(std::wstring (*source)()) { g_clipPathSource = source; }
    std::wstring CurrentClipPath() { return g_clipPathSource ? g_clipPathSource() : std::wstring(); }
    void NoteExtraFindings(std::vector<Finding> findings) { g_extraFindings = std::move(findings); }

    ModScope::ModScope() { ++t_modDepth; }
    ModScope::~ModScope() { --t_modDepth; }

    const std::vector<TickRow>& Rows() { return g_rows; }
    std::uint32_t RecordedPlayerId() { return g_playerId; }
    void SetPlayerSpeechProxy(bool on) { g_playerSpeechProxy = on; }
    const char* Phase() { return g_phase; }
    const std::vector<LiveSound>& Live() { return g_live; }

    bool OwnerOf(std::uint64_t audio, std::uint64_t tracker, Owner& out)
    {
        Match match = Match::None;
        const Owner* owner = g_owners.Find(audio, tracker, match);
        if (!owner)
            return false;
        out = *owner;
        return true;
    }

    bool ReadQword(std::uint64_t address, std::uint64_t& value)
    {
        return GuardedCopy(&value, reinterpret_cast<const void*>(address), 8);
    }

    bool ReadBytes(std::uint64_t address, void* out, std::size_t size)
    {
        return GuardedCopy(out, reinterpret_cast<const void*>(address), size);
    }

    std::uint64_t ImageBase() { return g_base; }
    std::uint64_t ImageEnd() { return g_imageEnd; }

}
