#include <intrin.h>
#include "soundfx_probe.h"

#include "guarded_memory.h"
#include "hook_batch.h"
#include "mod_identity.h"
#include "module_lifetime.h"
#include "profiler.h"
#include "thread_names.h"
#include "version.generated.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

// See soundfx_probe.h. Every address here is for build 1491.50.
//
// One hook per port. Every detour below reads for the probe (g_state,
// SoundFxProbe=1) and pushes for the generic sound recorder
// (soundgeneric::Capturing(), a recording with SoundGeneric=1); the ports
// the recorder needs (HookSpec::generic) are installed with either key on.

namespace replay::soundfx
{
    namespace
    {
        // ---- engine facts (build 1491.50) ----

        // The pools of sounds and of their requested settings: the same
        // three globals the delete path 0x2521648 reads. The probe uses them
        // only when these three instructions are the ones found offline.
        struct SiteCheck
        {
            std::uintptr_t rva;
            const char* bytes;
        };
        constexpr SiteCheck kPoolSites[] = {
            {0x25216A7, "0F AF 3D 02 72 E3 02"},    // imul edi, [sound stride]
            {0x25216AE, "48 8B 15 23 72 E3 02"},    // mov rdx, [pool table]
            {0x25216D8, "44 0F AF 05 D4 71 E3 02"}, // imul r8d, [settings stride]
        };
        constexpr std::uintptr_t kSoundStrideRva = 0x53588B0;
        constexpr std::uintptr_t kSettingsStrideRva = 0x53588B4;
        constexpr std::uintptr_t kPoolTableRva = 0x53588D8;
        constexpr std::uint64_t kPoolSize = 0x2A860;
        constexpr std::uint64_t kPoolSounds = 0x2A848;
        constexpr std::uint64_t kPoolSettings = 0x2A850;
        // The settings' write index every setting port reads first
        // (mov eax, [rip + d] at the volume port).
        constexpr std::uintptr_t kWriteIndexRva = 0x533852C;
        // Sound object fields.
        constexpr std::uint64_t kSoundTracker = 0x48;
        constexpr std::uint64_t kSoundAudio = 0x50;
        constexpr std::uint64_t kSoundBucket = 0xA0;
        constexpr std::uint64_t kSoundSettingsIndex = 0xD6;
        constexpr std::uint64_t kSoundParentIndex = 0xE2;
        // Every one of the 41 sound vtables has the variable setter S38 in
        // slot 1 (one implementation): the test for "this is a sound".
        constexpr std::uintptr_t kSoundVtableSlot1Rva = 0x360BF10 + 8;
        // Init params (sound_census_core.h has the rest).
        constexpr std::size_t kParamsBytes = 0x140;
        constexpr std::size_t kParamsVolume = 0xA4;
        constexpr std::size_t kParamsPitch = 0xAC;
        constexpr std::size_t kParamsWaveSlot = 0xD0;
        constexpr std::size_t kParamsVariables = 0x10C;
        constexpr std::size_t kParamsVariableCount = 0x124;
        // S15's four call sites (return addresses): the two creators give a
        // slot back when the sound could not be made, the two delete paths
        // when it ended.
        constexpr std::uint32_t kReleaseFromCreatorB = 0x2516C04;
        constexpr std::uint32_t kReleaseFromCreatorA = 0x2516FE2;
        // The speech's two calls of persistent-by-ref (through the thunk 0x2C2AA5C).
        constexpr std::uint32_t kSpeechByRefA = 0x2FA0DCC;
        constexpr std::uint32_t kSpeechByRefB = 0x2FA10CF;
        // The create-and-play queue's drain.
        constexpr std::uint32_t kDrainBegin = 0x258A348;
        constexpr std::uint32_t kDrainEnd = 0x258A546;
        // The horse's wet step layer.
        constexpr std::uint32_t kWetHoofSet = 0x0029934A;
        // Particle effect instance fields.
        constexpr std::uint64_t kInstColour = 0x380;
        constexpr std::uint64_t kInstAlpha = 0x38C;
        constexpr std::uint64_t kInstZoom = 0x394;
        constexpr std::uint64_t kInstEvolutions = 0x3E0;
        constexpr std::uint64_t kInstField3BF = 0x3BF;
        constexpr std::uint64_t kInstField5C0 = 0x5C0;
        constexpr std::uint64_t kInstField5CC = 0x5CC;

        constexpr wchar_t kReportFolder[] = L"ReplayDebug";
        constexpr double kReportEveryS = 60.0;
        constexpr std::uint64_t kNeverCreatedUs = 5000000; // a queued request this old never reached the creator

        using Fn1 = std::uint64_t(__fastcall*)(std::uint64_t);
        using Fn2 = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t);
        using Fn4 = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        using Fn5 = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        using Fn8 = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t, std::uint64_t);
        using FnF32 = std::uint64_t(__fastcall*)(std::uint64_t, float);
        using FnF32x2 = std::uint64_t(__fastcall*)(std::uint64_t, float, float);
        using FnHashF32 = std::uint64_t(__fastcall*)(std::uint64_t, const std::uint32_t*, float);

        struct State
        {
            PortStats ports[kPortCount];
            std::string status[kPortCount];
            KeyTable refs{8192};
            KeyTable speechRefs{1024};
            KeyTable speechVtables{64};
            KeyTable otherVtables{256};
            KeyTable soundVariables{2048};
            KeyTable entityVariables{1024};
            KeyTable evolutions{1024};
            // S40 hash -> value, the distinct values of P17 and P18.
            KeyTable globalVariables{4096};
            KeyTable fxField5CC{512};
            KeyTable fxFieldD94{512};
            KeyTable fx{8192};

            SRWLOCK lock = SRWLOCK_INIT;
            // Under `lock`:
            LiveTable live{8192};
            PendingQueue queue{2048};
            LifeStats life;
            std::vector<OwnerChange> ownerChanges;
            std::vector<WetHoofSample> hoofSamples;

            std::atomic<std::uint64_t> queueRequests{0};
            std::atomic<std::uint64_t> queueRefused{0};
            std::atomic<std::uint64_t> createdThroughDrain{0};
            std::atomic<std::uint64_t> createdOther{0};
            std::atomic<std::uint64_t> speechByRef{0};
            std::atomic<std::uint64_t> otherByRef{0};
            std::atomic<std::uint64_t> speechCreated{0};
            std::atomic<std::uint64_t> speechRefused{0};
            std::atomic<std::uint64_t> speechWithParams{0};
            std::atomic<std::uint64_t> speechWithWaveSlot{0};
            std::atomic<std::uint64_t> hoofRequests{0};
            std::atomic<std::uint64_t> hoofAccepted{0};
            std::atomic<std::uint64_t> hoofWithParams{0};
            std::atomic<std::uint64_t> hoofWithVariables{0};
            std::atomic<std::uint64_t> constructTop{0};
            std::atomic<std::uint64_t> constructChild{0};
            std::atomic<std::uint64_t> constructFailed{0};
            std::atomic<std::uint64_t> constructTopSettings{0};
            std::atomic<std::uint64_t> constructChildSettings{0};
            std::atomic<std::uint64_t> constructTopNoParent{0};
            std::atomic<std::uint64_t> constructChildNoParent{0};

            bool writeIndexKnown = false;
            std::string mismatches;

            // Script thread.
            std::uint64_t startTicks = 0;
            std::uint64_t lastTick = 0;
            std::uint64_t lastReport = 0;
            std::uint64_t lastExpire = 0;
            std::uint64_t modeTicks[kModes] = {};
            std::uint64_t frames = 0;
            std::uint32_t scriptThread = 0;
            std::string started;
            std::wstring reportPath;
        };

        struct Lock
        {
            explicit Lock(SRWLOCK& lock) noexcept : m_lock(lock) { AcquireSRWLockExclusive(&m_lock); }
            ~Lock() noexcept { ReleaseSRWLockExclusive(&m_lock); }
            Lock(const Lock&) = delete;
            Lock& operator=(const Lock&) = delete;

        private:
            SRWLOCK& m_lock;
        };

        int g_iniRead = 0;
        bool g_enabled = false;
        State* g_state = nullptr;
        bool g_poolsKnown = false; // kPoolSites matched (shared with the generic recorder)
        std::atomic<void*> g_originals[kPortCount] = {};
        void* g_targets[kPortCount] = {};
        bool g_hooksInstalled = false; // Install ran (with or without the probe's tables)
        std::atomic<std::uint8_t> g_mode{0};
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
        LARGE_INTEGER g_frequency{};
        thread_local int t_speechByRef = 0;

        std::uint64_t ToUs(std::uint64_t ticks)
        {
            // Whole seconds apart: ticks * 1e6 overflows after 21 days of uptime (mod_clock.h).
            const std::uint64_t frequency = static_cast<std::uint64_t>(g_frequency.QuadPart);
            return frequency ? ticks / frequency * 1000000ull + ticks % frequency * 1000000ull / frequency : 0;
        }

        double ToSeconds(std::uint64_t ticks)
        {
            return g_frequency.QuadPart ? static_cast<double>(ticks) / static_cast<double>(g_frequency.QuadPart) : 0.0;
        }

        std::uint32_t ToRva(std::uint64_t address)
        {
            return address >= g_base && address < g_imageEnd ? static_cast<std::uint32_t>(address - g_base) : 0;
        }

        std::uint32_t ToRva(void* address) { return ToRva(reinterpret_cast<std::uint64_t>(address)); }

        // Engine heap memory: below 4 GB or non-canonical is refused without
        // a fault (guarded_memory.h, the audio observer's rule).
        template <typename T>
        bool ReadHeap(std::uint64_t address, T& out)
        {
            return replay::mem::Copy(&out, reinterpret_cast<const void*>(address), sizeof(T), replay::mem::kGameHeapLowest);
        }

        // Image or stack memory (a hash or a vector passed by address).
        template <typename T>
        bool ReadAny(std::uint64_t address, T& out)
        {
            return replay::mem::Copy(&out, reinterpret_cast<const void*>(address), sizeof(T));
        }

        std::size_t CurrentMode() { return g_mode.load(std::memory_order_relaxed); }

        // The requested settings of a top-level sound (the lookup play, stop
        // and stop-and-forget inline); 0 when it has none or the pools are
        // not the ones found offline.
        std::uint64_t SettingsOf(std::uint64_t sound)
        {
            if (!g_poolsKnown)
                return 0;
            std::uint16_t index = 0xFFFF;
            std::uint8_t bucket = 0;
            std::uint32_t stride = 0;
            std::uint64_t table = 0;
            std::uint64_t pool = 0;
            if (!ReadHeap(sound + kSoundSettingsIndex, index) || index == 0xFFFF || !ReadHeap(sound + kSoundBucket, bucket) ||
                !ReadAny(g_base + kSettingsStrideRva, stride) || !ReadAny(g_base + kPoolTableRva, table) || !table ||
                !ReadHeap(table + bucket * kPoolSize + kPoolSettings, pool) || !pool)
            {
                return 0;
            }
            return pool + static_cast<std::uint64_t>(index) * stride;
        }

        // The sound of a manager slot (6 bytes: next, sound index, bucket),
        // as the delete path 0x2521648 finds it.
        std::uint64_t SoundOfSlot(std::uint64_t manager, std::uint32_t slot)
        {
            if (!g_poolsKnown || slot >= 0xFFFF)
                return 0;
            std::uint16_t index = 0;
            std::uint8_t bucket = 0;
            std::uint32_t stride = 0;
            std::uint64_t table = 0;
            std::uint64_t pool = 0;
            const std::uint64_t entry = manager + static_cast<std::uint64_t>(slot) * 6;
            if (!ReadAny(entry + 2, index) || !ReadAny(entry + 4, bucket) || !ReadAny(g_base + kSoundStrideRva, stride) ||
                !ReadAny(g_base + kPoolTableRva, table) || !table || !ReadHeap(table + bucket * kPoolSize + kPoolSounds, pool) ||
                !pool)
            {
                return 0;
            }
            return pool + static_cast<std::uint64_t>(index) * stride;
        }

        bool IsSound(std::uint64_t sound)
        {
            std::uint64_t vtable = 0;
            std::uint64_t slot1 = 0;
            return ReadHeap(sound, vtable) && ToRva(vtable) && ReadAny(vtable + 8, slot1) &&
                slot1 == g_base + Info(kS38).rva;
        }

        struct LiveInfo
        {
            bool known = false;
            std::uint64_t ref = 0;
            std::uint64_t owner = 0;
            bool hasParams = false;
            float paramVolume = 0.0f;
            std::int16_t paramPitch = 0;
        };

        // Under the lock: the owner fields of a live sound against what they
        // were (the question "does a live sound ever change owner").
        void CheckOwnerLocked(State& state, LiveSound& live, std::uint64_t audioNow, std::uint64_t trackerNow, Port port,
            std::uint32_t caller, std::uint64_t nowTicks)
        {
            ++state.life.ownerChecks;
            const bool first = live.ownerChanges == 0;
            const std::uint64_t ref = live.ref;
            const OwnerCheck check = CheckOwner(live, audioNow, trackerNow);
            if (!check.audioChanged && !check.trackerChanged)
                return;
            state.life.audioChanges += check.audioChanged ? 1 : 0;
            state.life.trackerChanges += check.trackerChanged ? 1 : 0;
            state.life.soundsWithChange += first ? 1 : 0;
            if (state.ownerChanges.size() < state.ownerChanges.capacity())
            {
                OwnerChange change;
                change.sound = live.sound;
                change.ref = ref;
                change.oldAudio = check.oldAudio;
                change.newAudio = audioNow;
                change.oldTracker = check.oldTracker;
                change.newTracker = trackerNow;
                change.caller = caller;
                change.port = port;
                change.ageS = static_cast<double>(ToUs(nowTicks) - live.createdUs) / 1000000.0;
                state.ownerChanges.push_back(change);
            }
        }

        // A call on a top-level sound: its entry, the owner check.
        LiveInfo Touch(State& state, Port port, std::uint64_t sound, std::uint32_t caller, bool setting, bool forget)
        {
            LiveInfo info;
            if (!sound)
                return info;
            std::uint64_t audioNow = 0;
            std::uint64_t trackerNow = 0;
            const bool readable = ReadHeap(sound + kSoundAudio, audioNow) && ReadHeap(sound + kSoundTracker, trackerNow);
            const std::uint64_t now = Ticks();
            Lock lock(state.lock);
            if (forget)
                ++state.life.forgetCalls;
            LiveSound* live = state.live.Find(sound);
            if (!live)
            {
                if (forget)
                    ++state.life.forgetUnknown;
                return info;
            }
            if (readable)
                CheckOwnerLocked(state, *live, audioNow, trackerNow, port, caller, now);
            if (setting)
                ++live->settings;
            if (forget)
                live->forgot = true;
            info.known = true;
            info.ref = live->ref;
            info.owner = live->audio;
            info.hasParams = live->hasParams;
            info.paramVolume = live->paramVolume;
            info.paramPitch = live->paramPitch;
            return info;
        }

        void AddSample(PortStats& stats, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, float value,
            std::uint64_t sound, const LiveInfo& info, std::uint32_t caller, std::uint32_t thread)
        {
            if (stats.samples.Full())
                return;
            Sample sample;
            sample.args[0] = a0;
            sample.args[1] = a1;
            sample.args[2] = a2;
            sample.args[3] = a3;
            sample.value = value;
            sample.sound = sound;
            sample.ref = info.ref;
            sample.owner = info.owner;
            sample.caller = caller;
            sample.thread = thread;
            stats.samples.Add(sample);
        }

        // Every setting port: rcx is the requested settings block, whose
        // first field points back to its sound.
        void NoteSetting(Port port, std::uint64_t block, double value, std::uint64_t raw, float sampleValue, void* returnAddress,
            bool second = false, double value2 = 0.0)
        {
            State* state = g_state;
            if (!state)
                return;
            PortStats& stats = state->ports[port];
            const std::uint32_t thread = GetCurrentThreadId();
            const std::uint32_t caller = ToRva(returnAddress);
            stats.Note(CurrentMode(), thread, caller);
            stats.values[0].Add(value);
            if (second)
                stats.values[1].Add(value2);
            // Probe 2: [block] is a sound and that sound's settings are this block.
            std::uint64_t sound = 0;
            if (!ReadHeap(block, sound) || !sound)
            {
                stats.linkUnread.fetch_add(1, std::memory_order_relaxed);
                sound = 0;
            }
            else if (IsSound(sound) && SettingsOf(sound) == block)
                stats.linkOk.fetch_add(1, std::memory_order_relaxed);
            else
            {
                stats.linkBad.fetch_add(1, std::memory_order_relaxed);
                sound = 0;
            }
            const LiveInfo info = Touch(*state, port, sound, caller, true, false);
            // Probe 4: the value against the same sound's creation params.
            if (info.known && info.hasParams && port == kS18)
                stats.values[1].Add(info.paramVolume);
            if (info.known && info.hasParams && port == kS19)
                stats.values[1].Add(info.paramPitch);
            if (port == kS18 && state->writeIndexKnown)
            {
                // The fields S31 and S32 write, read where the volume is written (beside
                // the two ports' own detours).
                std::uint32_t index = 0;
                std::uint16_t word = 0;
                float field = 0.0f;
                if (ReadAny(g_base + kWriteIndexRva, index) && index < 4)
                {
                    const std::uint64_t copy = block + (static_cast<std::uint64_t>(index) << 6);
                    if (ReadHeap(copy + 0x14C, word))
                        state->ports[kS31].values[1].Add(word & 0x1FF);
                    if (ReadHeap(copy + 0x138, field))
                        state->ports[kS32].values[1].Add(field);
                }
            }
            AddSample(stats, block, raw, 0, 0, sampleValue, sound, info, caller, thread);
        }

        // A port whose first argument is the sound (prepare, play, stop).
        void NoteSoundCall(Port port, std::uint64_t sound, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
            void* returnAddress)
        {
            State* state = g_state;
            if (!state)
                return;
            PortStats& stats = state->ports[port];
            const std::uint32_t thread = GetCurrentThreadId();
            const std::uint32_t caller = ToRva(returnAddress);
            stats.Note(CurrentMode(), thread, caller);
            const LiveInfo info = Touch(*state, port, sound, caller, false, false);
            AddSample(stats, sound, a1, a2, a3, 0.0f, sound, info, caller, thread);
        }

        std::uint32_t FloatBits(float value)
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, 4);
            return bits;
        }

        // The recorder's adjust kind of a setting port (-1: a port the recorder
        // does not keep).
        int GenericKindOf(Port port)
        {
            switch (port)
            {
            case kS16: return static_cast<int>(replay::soundgeneric::Adjust::Position);
            case kS17: return static_cast<int>(replay::soundgeneric::Adjust::PositionNoDoppler);
            case kS18: return static_cast<int>(replay::soundgeneric::Adjust::Volume);
            case kS19: return static_cast<int>(replay::soundgeneric::Adjust::Pitch);
            case kS21: return static_cast<int>(replay::soundgeneric::Adjust::LowPass);
            case kS22: return static_cast<int>(replay::soundgeneric::Adjust::HighPass);
            case kS23: return static_cast<int>(replay::soundgeneric::Adjust::PostSubmix);
            case kS24: return static_cast<int>(replay::soundgeneric::Adjust::VolumeCurveScale);
            default: return -1;
            }
        }

        // A setting call for the recorder: [block] is the sound (held in every one
        // of 13 million calls measured), one guarded read, then the ring.
        void GenericSetting(Port port, std::uint64_t block, const float* value)
        {
            const int kind = GenericKindOf(port);
            if (kind < 0)
                return;
            std::uint64_t sound = 0;
            if (!ReadHeap(block, sound) || !sound)
                return;
            replay::soundgeneric::OnAdjust(static_cast<replay::soundgeneric::Adjust>(kind), sound, value, 0);
        }

        // ---- detours: read, then the original with the same arguments ----

#define SOUNDFX_SETTING_F32(NAME, PORT)                                                              \
        std::uint64_t __fastcall NAME(std::uint64_t block, float value)                             \
        {                                                                                            \
            replay::lifetime::DetourScope lifetime; /* the whole call, first */                      \
            REPLAY_PROFILE_ZONE("detour.probe_sound_set");                                           \
            NoteSetting(PORT, block, value, FloatBits(value), value, _ReturnAddress());              \
            if (replay::soundgeneric::Capturing())                                                  \
                GenericSetting(PORT, block, &value);                                                 \
            const FnF32 original = reinterpret_cast<FnF32>(g_originals[PORT].load(std::memory_order_acquire)); \
            return original ? original(block, value) : 0;                                            \
        }

#define SOUNDFX_SETTING_INT(NAME, PORT, EXPRESSION)                                                  \
        std::uint64_t __fastcall NAME(std::uint64_t block, std::uint64_t argument)                  \
        {                                                                                            \
            replay::lifetime::DetourScope lifetime; /* the whole call, first */                      \
            REPLAY_PROFILE_ZONE("detour.probe_sound_set");                                           \
            const double value = static_cast<double>(EXPRESSION);                                    \
            NoteSetting(PORT, block, value, argument, static_cast<float>(value), _ReturnAddress());  \
            if (replay::soundgeneric::Capturing())                                                  \
            {                                                                                        \
                const float narrow = static_cast<float>(value);                                      \
                GenericSetting(PORT, block, &narrow);                                                \
            }                                                                                        \
            const Fn2 original = reinterpret_cast<Fn2>(g_originals[PORT].load(std::memory_order_acquire)); \
            return original ? original(block, argument) : 0;                                         \
        }

#define SOUNDFX_SETTING_VEC3(NAME, PORT)                                                             \
        std::uint64_t __fastcall NAME(std::uint64_t block, std::uint64_t vector)                    \
        {                                                                                            \
            replay::lifetime::DetourScope lifetime; /* the whole call, first */                      \
            REPLAY_PROFILE_ZONE("detour.probe_sound_set");                                           \
            float xyz[3] = {};                                                                       \
            ReadAny(vector, xyz);                                                                    \
            NoteSetting(PORT, block, xyz[0], (static_cast<std::uint64_t>(FloatBits(xyz[1])) << 32) | FloatBits(xyz[0]), \
                xyz[2], _ReturnAddress(), true, xyz[2]);                                             \
            if (replay::soundgeneric::Capturing())                                                  \
                GenericSetting(PORT, block, xyz);                                                    \
            const Fn2 original = reinterpret_cast<Fn2>(g_originals[PORT].load(std::memory_order_acquire)); \
            return original ? original(block, vector) : 0;                                           \
        }

        SOUNDFX_SETTING_VEC3(PositionDetour, kS16)
        SOUNDFX_SETTING_VEC3(PositionNoDopplerDetour, kS17)
        SOUNDFX_SETTING_F32(VolumeDetour, kS18)
        SOUNDFX_SETTING_INT(PitchDetour, kS19, static_cast<std::int16_t>(argument))
        SOUNDFX_SETTING_INT(PanDetour, kS20, static_cast<std::int16_t>(argument))
        SOUNDFX_SETTING_INT(LowPassDetour, kS21, static_cast<std::uint32_t>(argument))
        SOUNDFX_SETTING_INT(HighPassDetour, kS22, static_cast<std::uint32_t>(argument))
        SOUNDFX_SETTING_F32(PostSubmixDetour, kS23)
        SOUNDFX_SETTING_F32(VolumeCurveScaleDetour, kS24)
        SOUNDFX_SETTING_F32(DopplerDetour, kS25)
        SOUNDFX_SETTING_INT(EnvironmentalLoudnessU8Detour, kS26, static_cast<std::uint8_t>(argument))
        SOUNDFX_SETTING_F32(EnvironmentalLoudnessFloatDetour, kS27)
        SOUNDFX_SETTING_F32(FrequencySmoothingDetour, kS29)
        SOUNDFX_SETTING_INT(EnvironmentGroupDetour, kS30, argument == 0 ? 1 : 0)
        SOUNDFX_SETTING_INT(SpeakerMaskDetour, kS33, static_cast<std::uint8_t>(argument))
        SOUNDFX_SETTING_INT(ListenerMaskDetour, kS34, static_cast<std::uint8_t>(argument))
        SOUNDFX_SETTING_INT(DistanceAttenuationDetour, kS35, static_cast<std::uint8_t>(argument))
        SOUNDFX_SETTING_INT(RearAttenuationDetour, kS36, static_cast<std::uint8_t>(argument))
        SOUNDFX_SETTING_INT(RearFilterDetour, kS37, static_cast<std::uint8_t>(argument))
        // S31 (block, int; -1 is stored as 0x1FF) and S32 (block, float), the two
        // the init params' copy 0x258A9E8 calls with the params' +0xAE (pan) and
        // +0xBC.
        SOUNDFX_SETTING_INT(PanFromParamsDetour, kS31, static_cast<std::int32_t>(argument))
        SOUNDFX_SETTING_F32(FloatFromParamsDetour, kS32)

        std::uint64_t __fastcall SourceEffectMixDetour(std::uint64_t block, float wet, float dry)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_sound_set");
            NoteSetting(kS28, block, wet, (static_cast<std::uint64_t>(FloatBits(dry)) << 32) | FloatBits(wet), wet,
                _ReturnAddress(), true, dry);
            const FnF32x2 original = reinterpret_cast<FnF32x2>(g_originals[kS28].load(std::memory_order_acquire));
            return original ? original(block, wet, dry) : 0;
        }

        // S38 (sound, hash*, value) and S39 (variable block, hash*, value).
        void NoteVariable(Port port, KeyTable& table, std::uint64_t object, const std::uint32_t* hashAddress, float value,
            bool isSound, void* returnAddress)
        {
            State* state = g_state;
            if (!state)
                return;
            PortStats& stats = state->ports[port];
            const std::uint32_t thread = GetCurrentThreadId();
            const std::uint32_t caller = ToRva(returnAddress);
            stats.Note(CurrentMode(), thread, caller);
            stats.values[0].Add(value);
            std::uint32_t hash = 0;
            if (ReadAny(reinterpret_cast<std::uint64_t>(hashAddress), hash))
                table.AddValue(hash, value);
            const LiveInfo info = isSound ? Touch(*state, port, object, caller, true, false) : LiveInfo{};
            AddSample(stats, object, (static_cast<std::uint64_t>(FloatBits(value)) << 32) | hash, 0, 0, value,
                isSound ? object : 0, info, caller, thread);
        }

        std::uint64_t __fastcall SoundVariableDetour(std::uint64_t sound, const std::uint32_t* hash, float value)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_sound_set");
            if (State* state = g_state)
                NoteVariable(kS38, state->soundVariables, sound, hash, value, true, _ReturnAddress());
            // The hash is read before the call (the caller's storage), and the packet
            // is written only when the engine took the value (al true): S38 never adds
            // a variable, and a refused call changed nothing in the game, so replaying
            // it would fail again.
            std::uint32_t key = 0;
            const bool capturing = replay::soundgeneric::Capturing() && sound &&
                ReadAny(reinterpret_cast<std::uint64_t>(hash), key);
            const FnHashF32 original = reinterpret_cast<FnHashF32>(g_originals[kS38].load(std::memory_order_acquire));
            const std::uint64_t result = original ? original(sound, hash, value) : 0;
            if (capturing) // the first argument is the sound itself
            {
                if (result & 0xFF)
                    replay::soundgeneric::OnAdjust(replay::soundgeneric::Adjust::Variable, sound, &value, key);
                else
                    replay::soundgeneric::OnVariableRefused();
            }
            return result;
        }

        std::uint64_t __fastcall EntityVariableDetour(std::uint64_t block, const std::uint32_t* hash, float value)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_sound_set");
            if (State* state = g_state)
                NoteVariable(kS39, state->entityVariables, block, hash, value, false, _ReturnAddress());
            const FnHashF32 original = reinterpret_cast<FnHashF32>(g_originals[kS39].load(std::memory_order_acquire));
            return original ? original(block, hash, value) : 0;
        }

        // S10 (sound, wave slot, may load, priority) -> state 0, 1 or 2.
        std::uint64_t __fastcall PrepareDetour(std::uint64_t sound, std::uint64_t waveSlot, std::uint64_t mayLoad,
            std::uint64_t priority)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_sound_life");
            NoteSoundCall(kS10, sound, waveSlot, mayLoad, priority, _ReturnAddress());
            const Fn4 original = reinterpret_cast<Fn4>(g_originals[kS10].load(std::memory_order_acquire));
            const std::uint64_t result = original ? original(sound, waveSlot, mayLoad, priority) : 0;
            if (State* state = g_state)
            {
                state->ports[kS10].values[0].Add(static_cast<std::int32_t>(result));
                state->ports[kS10].values[1].Add(static_cast<std::int32_t>(priority));
            }
            return result;
        }

        // S11 (sound, wave slot, may load, int, byte): five arguments, as the
        // mod's own call of it (audio_observer.cpp PrepareAndPlayFn).
        std::uint64_t __fastcall PrepareAndPlayDetour(std::uint64_t sound, std::uint64_t waveSlot, std::uint64_t mayLoad,
            std::uint64_t argument3, std::uint64_t argument4)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_sound_life");
            NoteSoundCall(kS11, sound, waveSlot, mayLoad, argument3, _ReturnAddress());
            if (State* state = g_state)
            {
                state->ports[kS11].values[0].Add(static_cast<std::int32_t>(argument3));
                state->ports[kS11].values[1].Add(waveSlot ? 1 : 0);
            }
            const Fn5 original = reinterpret_cast<Fn5>(g_originals[kS11].load(std::memory_order_acquire));
            return original ? original(sound, waveSlot, mayLoad, argument3, argument4) : 0;
        }

        std::uint64_t __fastcall PlayDetour(std::uint64_t sound)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_sound_life");
            NoteSoundCall(kS12, sound, 0, 0, 0, _ReturnAddress());
            const Fn1 original = reinterpret_cast<Fn1>(g_originals[kS12].load(std::memory_order_acquire));
            return original ? original(sound) : 0;
        }

        std::uint64_t __fastcall StopDetour(std::uint64_t sound)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_sound_life");
            NoteSoundCall(kS13, sound, 0, 0, 0, _ReturnAddress());
            if (replay::soundgeneric::Capturing()) // the stop packet
                replay::soundgeneric::OnStop(sound, false);
            const Fn1 original = reinterpret_cast<Fn1>(g_originals[kS13].load(std::memory_order_acquire));
            return original ? original(sound) : 0;
        }

        // S15 (manager, slot): probe 13. The slot's sound is read before the
        // original gives the slot back.
        std::uint64_t __fastcall SlotReleaseDetour(std::uint64_t manager, std::uint64_t slot)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_sound_life");
            // The slot's sound, read once before the release for the probe and the
            // recorder; a creator's failure path gives a slot back that never held a
            // sound.
            const std::uint32_t caller = ToRva(_ReturnAddress());
            const bool createFailed = caller == kReleaseFromCreatorA || caller == kReleaseFromCreatorB;
            const bool generic = replay::soundgeneric::Capturing();
            const std::uint64_t sound = createFailed || (!g_state && !generic) ? 0 : SoundOfSlot(manager, static_cast<std::uint32_t>(slot));
            if (generic && sound)
                replay::soundgeneric::OnRelease(sound);
            if (State* state = g_state)
            {
                PortStats& stats = state->ports[kS15];
                const std::uint32_t thread = GetCurrentThreadId();
                stats.Note(CurrentMode(), thread, caller);
                std::uint64_t audioNow = 0;
                std::uint64_t trackerNow = 0;
                const bool readable = sound && ReadHeap(sound + kSoundAudio, audioNow) &&
                    ReadHeap(sound + kSoundTracker, trackerNow);
                const std::uint64_t now = Ticks();
                LiveSound ended;
                bool known = false;
                {
                    Lock lock(state->lock);
                    if (createFailed)
                        ++state->life.releaseCreateFailed;
                    else if (!sound)
                        ++state->life.releaseUnmapped;
                    else
                    {
                        if (LiveSound* live = state->live.Find(sound))
                        {
                            if (readable)
                                CheckOwnerLocked(*state, *live, audioNow, trackerNow, kS15, caller, now);
                            known = state->live.Erase(sound, &ended);
                        }
                        if (!known)
                            ++state->life.releaseUnknown;
                        else if (ended.forgot)
                            ++state->life.releaseAfterForget;
                        else
                            ++state->life.releaseWithoutForget;
                    }
                }
                LiveInfo info;
                if (known)
                {
                    info.known = true;
                    info.ref = ended.ref;
                    info.owner = ended.audio;
                    stats.values[ended.forgot ? 0 : 1].Add(static_cast<double>(ToUs(now) - ended.createdUs) / 1000000.0);
                }
                AddSample(stats, manager, slot, known ? (ended.forgot ? 1 : 0) : 2, ended.settings, 0.0f, sound, info, caller,
                    thread);
            }
            const Fn2 original = reinterpret_cast<Fn2>(g_originals[kS15].load(std::memory_order_acquire));
            return original ? original(manager, slot) : 0;
        }

        // 0x252AAE0, the construction every sound goes through (8 arguments:
        // it reads its frame up to entry rsp+0x40), the parent in r8: probe 3.
        std::uint64_t __fastcall ConstructDetour(std::uint64_t a1, std::uint64_t a2, std::uint64_t parent, std::uint64_t a4,
            std::uint64_t a5, std::uint64_t a6, std::uint64_t a7, std::uint64_t a8)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_sound_life");
            const Fn8 original = reinterpret_cast<Fn8>(g_originals[kConstruct].load(std::memory_order_acquire));
            const std::uint64_t sound = original ? original(a1, a2, parent, a4, a5, a6, a7, a8) : 0;
            if (State* state = g_state)
            {
                PortStats& stats = state->ports[kConstruct];
                const std::uint32_t thread = GetCurrentThreadId();
                const std::uint32_t caller = ToRva(_ReturnAddress());
                stats.Note(CurrentMode(), thread, caller);
                std::uint16_t settings = 0xFFFF;
                std::uint16_t parentIndex = 0xFFFF;
                if (!sound || !ReadHeap(sound + kSoundSettingsIndex, settings) || !ReadHeap(sound + kSoundParentIndex, parentIndex))
                    state->constructFailed.fetch_add(1, std::memory_order_relaxed);
                else if (!parent)
                {
                    state->constructTop.fetch_add(1, std::memory_order_relaxed);
                    if (settings != 0xFFFF)
                        state->constructTopSettings.fetch_add(1, std::memory_order_relaxed);
                    if (parentIndex == 0xFFFF)
                        state->constructTopNoParent.fetch_add(1, std::memory_order_relaxed);
                    stats.values[0].Add(parentIndex);
                }
                else
                {
                    state->constructChild.fetch_add(1, std::memory_order_relaxed);
                    if (settings != 0xFFFF)
                        state->constructChildSettings.fetch_add(1, std::memory_order_relaxed);
                    if (parentIndex == 0xFFFF)
                        state->constructChildNoParent.fetch_add(1, std::memory_order_relaxed);
                    stats.values[1].Add(parentIndex);
                }
                AddSample(stats, a1, parent ? 1 : 0, (static_cast<std::uint64_t>(parentIndex) << 16) | settings, parent, 0.0f,
                    sound, LiveInfo{}, caller, thread);
            }
            return sound;
        }

        // S40 (sound manager, hash, value in xmm2, create flag in r9b): the
        // manager's variable of that hash. Probe 12.
        using FnHashValue = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, float, std::uint64_t);
        std::uint64_t __fastcall GlobalVariableDetour(std::uint64_t manager, std::uint64_t hash, float value,
            std::uint64_t create)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_sound_set");
            if (State* state = g_state)
            {
                PortStats& stats = state->ports[kS40];
                const std::uint32_t thread = GetCurrentThreadId();
                const std::uint32_t caller = ToRva(_ReturnAddress());
                stats.Note(CurrentMode(), thread, caller);
                stats.values[0].Add(value);
                stats.values[1].Add(static_cast<std::uint8_t>(create) ? 1 : 0);
                state->globalVariables.AddValue(static_cast<std::uint32_t>(hash), value, true);
                AddSample(stats, manager, (static_cast<std::uint64_t>(FloatBits(value)) << 32) | static_cast<std::uint32_t>(hash),
                    ToRva(manager), static_cast<std::uint8_t>(create), value, 0, LiveInfo{}, caller, thread);
            }
            const FnHashValue original = reinterpret_cast<FnHashValue>(g_originals[kS40].load(std::memory_order_acquire));
            return original ? original(manager, hash, value, create) : 0;
        }

        // P17 (instance, int): the int kept at +0x5CC. P18 (instance, u16): the
        // word kept at [+0x5C0]+0xD94.
        std::uint64_t __fastcall FxField5CCDetour(std::uint64_t instance, std::uint64_t argument)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_fx");
            if (State* state = g_state)
            {
                PortStats& stats = state->ports[kP17];
                const std::uint32_t thread = GetCurrentThreadId();
                const std::uint32_t caller = ToRva(_ReturnAddress());
                stats.Note(CurrentMode(), thread, caller);
                const std::uint32_t value = static_cast<std::uint32_t>(argument);
                stats.values[0].Add(value);
                std::uint32_t before = 0;
                if (ReadHeap(instance + kInstField5CC, before))
                    stats.values[1].Add(before != value ? 1 : 0);
                state->fxField5CC.Add(value);
                AddSample(stats, instance, value, before, 0, 0.0f, 0, LiveInfo{}, caller, thread);
            }
            const Fn2 original = reinterpret_cast<Fn2>(g_originals[kP17].load(std::memory_order_acquire));
            return original ? original(instance, argument) : 0;
        }

        std::uint64_t __fastcall FxFieldD94Detour(std::uint64_t instance, std::uint64_t argument)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_fx");
            if (State* state = g_state)
            {
                PortStats& stats = state->ports[kP18];
                const std::uint32_t thread = GetCurrentThreadId();
                const std::uint32_t caller = ToRva(_ReturnAddress());
                stats.Note(CurrentMode(), thread, caller);
                const std::uint16_t value = static_cast<std::uint16_t>(argument);
                stats.values[0].Add(value);
                state->fxFieldD94.Add(value);
                AddSample(stats, instance, value, 0, 0, 0.0f, 0, LiveInfo{}, caller, thread);
            }
            const Fn2 original = reinterpret_cast<Fn2>(g_originals[kP18].load(std::memory_order_acquire));
            return original ? original(instance, argument) : 0;
        }

        // P12 (instance, hash*, value 0..1): probe 10.
        std::uint64_t __fastcall EvolutionDetour(std::uint64_t instance, const std::uint32_t* hashAddress, float value)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_fx");
            if (State* state = g_state)
            {
                PortStats& stats = state->ports[kP12];
                const std::uint32_t thread = GetCurrentThreadId();
                const std::uint32_t caller = ToRva(_ReturnAddress());
                stats.Note(CurrentMode(), thread, caller);
                stats.values[0].Add(value);
                std::uint32_t hash = 0;
                if (ReadAny(reinterpret_cast<std::uint64_t>(hashAddress), hash))
                    state->evolutions.AddValue(hash, value);
                AddSample(stats, instance, (static_cast<std::uint64_t>(FloatBits(value)) << 32) | hash, 0, 0, value, 0,
                    LiveInfo{}, caller, thread);
            }
            const FnHashF32 original = reinterpret_cast<FnHashF32>(g_originals[kP12].load(std::memory_order_acquire));
            return original ? original(instance, hashAddress, value) : 0;
        }

        // P13 (instance, colour*).
        std::uint64_t __fastcall ColourDetour(std::uint64_t instance, std::uint64_t colour)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_fx");
            if (State* state = g_state)
            {
                PortStats& stats = state->ports[kP13];
                const std::uint32_t thread = GetCurrentThreadId();
                const std::uint32_t caller = ToRva(_ReturnAddress());
                stats.Note(CurrentMode(), thread, caller);
                float rgb[3] = {};
                if (ReadAny(colour, rgb))
                {
                    stats.values[0].Add(rgb[0]);
                    stats.values[1].Add(rgb[2]);
                }
                AddSample(stats, instance, (static_cast<std::uint64_t>(FloatBits(rgb[1])) << 32) | FloatBits(rgb[0]), 0, 0,
                    rgb[2], 0, LiveInfo{}, caller, thread);
            }
            const Fn2 original = reinterpret_cast<Fn2>(g_originals[kP13].load(std::memory_order_acquire));
            return original ? original(instance, colour) : 0;
        }

        // P14 (instance, alpha).
        std::uint64_t __fastcall AlphaDetour(std::uint64_t instance, float alpha)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.probe_fx");
            if (State* state = g_state)
            {
                PortStats& stats = state->ports[kP14];
                const std::uint32_t thread = GetCurrentThreadId();
                const std::uint32_t caller = ToRva(_ReturnAddress());
                stats.Note(CurrentMode(), thread, caller);
                stats.values[0].Add(alpha);
                AddSample(stats, instance, FloatBits(alpha), 0, 0, alpha, 0, LiveInfo{}, caller, thread);
            }
            const FnF32 original = reinterpret_cast<FnF32>(g_originals[kP14].load(std::memory_order_acquire));
            return original ? original(instance, alpha) : 0;
        }

        // ---- install ----

        // The signatures: the port's first instructions in build 1491.50,
        // each found once in the dump's two code sections (the two that are
        // found twice carry a second check that makes them one).
        struct HookSpec
        {
            Port port;
            const char* signature;
            void* detour;
            SiteCheck check; // rva 0: none
            bool generic = false; // a port the generic sound recorder needs
        };

        const HookSpec kHooks[] = {
            {kS10, "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 30 33 DB 41 8B E9 45 8A F0",
                reinterpret_cast<void*>(&PrepareDetour), {0, nullptr}},
            {kS11, "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 33 DB 41 8B F9 45 8A F0",
                reinterpret_cast<void*>(&PrepareAndPlayDetour), {0, nullptr}},
            {kS12, "48 89 5C 24 10 57 48 83 EC 40 48 8B F9 0F B7 89 D6 00 00 00", reinterpret_cast<void*>(&PlayDetour),
                {0, nullptr}},
            // Found twice with the displacements masked (the other copy,
            // 0x3117E35, has no caller): the tail jump to 0x2536BEC makes it one.
            {kS13, "0F B7 91 D6 00 00 00 81 FA FF FF 00 00 74 2A 0F B6 81 A0 00 00 00 44 8B 05 ?? ?? ?? ?? 48 69 C8 60 A8 02 00 "
                   "44 0F AF C2 48 8B 05 ?? ?? ?? ?? 4C 03 84 01 50 A8 02 00",
                reinterpret_cast<void*>(&StopDetour), {0x2585C25, "E9 C2 0F FB FF"}, true},
            {kS15, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D B9 28 F8 00 00",
                reinterpret_cast<void*>(&SlotReleaseDetour), {0, nullptr}, true},
            {kS16, "8B 05 ?? ?? ?? ?? 0F 10 02 48 FF C0 48 03 C0 0F 11 04 C1 C3", reinterpret_cast<void*>(&PositionDetour),
                {0, nullptr}, true},
            {kS17, "8B 05 ?? ?? ?? ?? 0F 10 02 48 FF C0 BA 00 80 00 00", reinterpret_cast<void*>(&PositionNoDopplerDetour),
                {0, nullptr}, true},
            {kS18, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 F3 0F 11 8C 08 20 01 00 00 C3", reinterpret_cast<void*>(&VolumeDetour),
                {0, nullptr}, true},
            {kS19, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 66 89 94 08 3C 01 00 00 C3", reinterpret_cast<void*>(&PitchDetour),
                {0, nullptr}, true},
            {kS20, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 66 89 94 08 3E 01 00 00 C3", reinterpret_cast<void*>(&PanDetour),
                {0, nullptr}},
            {kS21, "41 B8 64 00 00 00 41 3B D0 72 0D 41 B8 20 4E 00 00 41 3B D0 44 0F 46 C2",
                reinterpret_cast<void*>(&LowPassDetour), {0, nullptr}, true},
            {kS22, "8B 05 ?? ?? ?? ?? 41 B8 5C 5D 00 00 41 3B D0 44 0F 46 C2", reinterpret_cast<void*>(&HighPassDetour),
                {0, nullptr}, true},
            {kS23, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 F3 0F 11 8C 08 24 01 00 00 C3", reinterpret_cast<void*>(&PostSubmixDetour),
                {0, nullptr}, true},
            {kS24, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 F3 0F 11 8C 08 34 01 00 00 C3",
                reinterpret_cast<void*>(&VolumeCurveScaleDetour), {0, nullptr}, true},
            {kS25, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 F3 0F 11 8C 08 30 01 00 00 C3", reinterpret_cast<void*>(&DopplerDetour),
                {0, nullptr}},
            {kS26, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 88 94 08 49 01 00 00 C3",
                reinterpret_cast<void*>(&EnvironmentalLoudnessU8Detour), {0, nullptr}},
            {kS27, "8B 05 ?? ?? ?? ?? F3 0F 59 0D ?? ?? ?? ?? 48 C1 E0 06 F3 0F 2C D1 88 94 08 49 01 00 00 C3",
                reinterpret_cast<void*>(&EnvironmentalLoudnessFloatDetour), {0, nullptr}},
            {kS28, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 F3 0F 11 8C 08 28 01 00 00 8B 05 ?? ?? ?? ?? 48 C1 E0 06 F3 0F 11 94 08 2C 01 "
                   "00 00 C3",
                reinterpret_cast<void*>(&SourceEffectMixDetour), {0, nullptr}},
            // The same nine bytes are inside another function (0x40B839, no
            // caller): a known call site of this one makes it one.
            {kS29, "F3 0F 11 89 28 02 00 00 C3", reinterpret_cast<void*>(&FrequencySmoothingDetour),
                {0x30FEDF9, "E8 AE 7B 43 FF"}},
            {kS30, "48 8B 81 48 02 00 00 48 85 C0 74 0F F0 FF 88 98 01 00 00", reinterpret_cast<void*>(&EnvironmentGroupDetour),
                {0, nullptr}},
            // The five ports proved in the game binary.
            {kS31, "41 B9 FF 01 00 00 83 FA FF 75 04 41 0F B7 D1 8B 05 ?? ?? ?? ?? 41 B8 00 FE 00 00 48 C1 E0 06",
                reinterpret_cast<void*>(&PanFromParamsDetour), {0x258AAEA, "E8 C1 C1 FA FF"}},
            {kS32, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 F3 0F 11 8C 08 38 01 00 00 C3", reinterpret_cast<void*>(&FloatFromParamsDetour),
                {0x258AAB0, "E8 B3 C4 FA FF"}},
            {kS40, "48 83 EC 38 48 8D 81 D0 96 00 00 45 8A C1 4C 8D 89 B0 96 00 00 48 89 44 24 20 8B CA 0F 28 CA",
                reinterpret_cast<void*>(&GlobalVariableDetour), {0, nullptr}},
            {kP17, "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 3B 91 CC 05 00 00",
                reinterpret_cast<void*>(&FxField5CCDetour), {0, nullptr}},
            {kP18, "48 89 5C 24 08 57 48 83 EC 40 48 8B F9 0F B7 DA 48 8B 0D ?? ?? ?? ?? 48 83 B9 C0 DF 03 00 00",
                reinterpret_cast<void*>(&FxFieldD94Detour), {0, nullptr}},
            {kS33, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 88 94 08 48 01 00 00 C3", reinterpret_cast<void*>(&SpeakerMaskDetour),
                {0, nullptr}},
            {kS34, "8B 05 ?? ?? ?? ?? 48 C1 E0 06 88 94 08 4A 01 00 00 C3", reinterpret_cast<void*>(&ListenerMaskDetour),
                {0, nullptr}},
            {kS35, "80 A1 5F 02 00 00 F3 C0 E2 02 08 91 5F 02 00 00 C3", reinterpret_cast<void*>(&DistanceAttenuationDetour),
                {0, nullptr}},
            {kS36, "80 A1 5F 02 00 00 CF C0 E2 04 08 91 5F 02 00 00 C3", reinterpret_cast<void*>(&RearAttenuationDetour),
                {0, nullptr}},
            {kS37, "80 A1 5F 02 00 00 3F C0 E2 06 08 91 5F 02 00 00 C3", reinterpret_cast<void*>(&RearFilterDetour),
                {0, nullptr}},
            {kS38, "48 83 EC 38 41 B8 00 00 80 7F 0F 29 74 24 20 0F 28 F2", reinterpret_cast<void*>(&SoundVariableDetour),
                {0, nullptr}, true},
            {kS39, "48 89 5C 24 10 4C 8D 41 10 4C 8B D9 49 63 98 80 00 00 00", reinterpret_cast<void*>(&EntityVariableDetour),
                {0, nullptr}},
            {kConstruct, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 49 8B F9 49 8B E8 48 8B F2 48 85 D2",
                reinterpret_cast<void*>(&ConstructDetour), {0, nullptr}},
            {kP12, "40 53 48 83 EC 30 48 8B 01 48 8B D9 0F 29 74 24 20 0F 28 F2 48 85 C0",
                reinterpret_cast<void*>(&EvolutionDetour), {0, nullptr}},
            {kP13, "F3 0F 6F 0A 0F 28 C1 0F C6 81 80 03 00 00 FE 0F C6 C8 84 0F 11 89 80 03 00 00 C3",
                reinterpret_cast<void*>(&ColourDetour), {0, nullptr}},
            {kP14, "F3 0F 11 89 8C 03 00 00 C3", reinterpret_cast<void*>(&AlphaDetour), {0, nullptr}},
        };

        bool BytesMatch(std::uintptr_t address, const char* text)
        {
            const Signature signature = ParseSignature(text);
            if (!signature.valid || signature.bytes.size() > 96)
                return false;
            std::uint8_t code[96] = {};
            return replay::mem::Copy(code, address, signature.bytes.size()) && MatchSignature(code, signature);
        }

        // "installed", or why the port stays off.
        const char* HookPort(const HookSpec& spec)
        {
            const std::uintptr_t address = g_base + Info(spec.port).rva;
            if (!BytesMatch(address, spec.signature))
                return "signature_mismatch";
            if (spec.check.rva && !BytesMatch(g_base + spec.check.rva, spec.check.bytes))
                return "second_check_mismatch";
            if (spec.port == kS38)
            {
                // The sound vtables' slot 1 is this function.
                std::uint64_t slot1 = 0;
                if (!ReadAny(g_base + kSoundVtableSlot1Rva, slot1) || slot1 != address)
                    return "vtable_slot_mismatch";
            }
            void* target = reinterpret_cast<void*>(address);
            void* trampoline = nullptr;
            if (MH_CreateHook(target, spec.detour, &trampoline) != MH_OK || !trampoline)
                return "create_failed";
            g_originals[spec.port].store(trampoline, std::memory_order_release);
            if (replay::hooks::Enable(target) != MH_OK)
            {
                MH_RemoveHook(target);
                g_originals[spec.port].store(nullptr, std::memory_order_release);
                return "enable_failed";
            }
            g_targets[spec.port] = target;
            return "installed";
        }

        void ReadIni()
        {
            if (g_iniRead)
                return;
            g_iniRead = 1;
            char directory[MAX_PATH]{};
            GetCurrentDirectoryA(MAX_PATH, directory);
            const std::string ini = replay::identity::IniPathIn(directory);
            g_enabled = replay::identity::DevIniInt("SoundFxProbe", 0, ini.c_str()) != 0;
        }

        std::string LocalTime(const SYSTEMTIME& time)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d", time.wYear, time.wMonth, time.wDay, time.wHour,
                time.wMinute, time.wSecond);
            return text;
        }

        // The mod's own name for a thread, else the description its owner
        // gave it (the game names some of its threads), else "".
        std::string ThreadNameOf(const State& state, std::uint32_t id)
        {
            const char* own = replay::threads::Lookup(id);
            if (own && *own)
                return own;
            std::string name;
            using GetFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
            static GetFn get = reinterpret_cast<GetFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
            if (get)
            {
                if (HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id))
                {
                    PWSTR wide = nullptr;
                    if (SUCCEEDED(get(thread, &wide)) && wide)
                    {
                        for (const wchar_t* c = wide; *c && name.size() < 48; ++c)
                            name += *c >= 32 && *c < 127 && *c != ',' && *c != '=' ? static_cast<char>(*c) : '_';
                        LocalFree(wide);
                    }
                    CloseHandle(thread);
                }
            }
            if (name.empty() && id == state.scriptThread)
                name = "script";
            return name;
        }

        void WriteReport(State& state, bool final)
        {
            REPLAY_PROFILE_ZONE("probe.soundfx_report");
            const std::uint64_t now = Ticks();
            ReportData data;
            data.version = replay::build::kVersion;
            data.started = state.started;
            SYSTEMTIME local{};
            GetLocalTime(&local);
            data.written = LocalTime(local);
            data.final = final;
            data.elapsedS = ToSeconds(now - state.startTicks);
            for (std::size_t mode = 0; mode < kModes; ++mode)
                data.modeS[mode] = ToSeconds(state.modeTicks[mode]);
            data.frames = state.frames;
            data.build = state.mismatches.empty() ? std::string("1491.50") : "differs:" + state.mismatches;
            std::vector<std::uint32_t> threads;
            for (const PortInfo& info : kPorts)
            {
                PortReport port = Snapshot(info.port, state.ports[info.port], state.status[info.port]);
                if (info.access == Access::Tap && port.Total() == 0)
                    port.status = "tap_not_seen";
                for (const std::uint32_t id : port.threads)
                    if (std::find(threads.begin(), threads.end(), id) == threads.end())
                        threads.push_back(id);
                data.ports.push_back(std::move(port));
            }
            std::sort(threads.begin(), threads.end());
            for (const std::uint32_t id : threads)
                data.threads.push_back({id, ThreadNameOf(state, id)});
            data.refs = state.refs.Snapshot();
            data.refOverflow = state.refs.Overflow();
            {
                Lock lock(state.lock);
                data.life = state.life;
                data.life.liveNow = state.live.Size();
                data.ownerChanges = state.ownerChanges;
                data.queue.enqueued = state.queue.Enqueued();
                data.queue.matched = state.queue.Matched();
                data.queue.neverCreated = state.queue.Expired();
                data.queue.overflow = state.queue.Overflow();
                data.queue.waiting = state.queue.Waiting();
                data.wetHoof.samples = state.hoofSamples;
            }
            data.queue.requests = state.queueRequests.load(std::memory_order_relaxed);
            data.queue.refused = state.queueRefused.load(std::memory_order_relaxed);
            data.queue.createdThroughDrain = state.createdThroughDrain.load(std::memory_order_relaxed);
            data.queue.createdOther = state.createdOther.load(std::memory_order_relaxed);
            data.queue.paramsBuilt = state.ports[kParams].Total();
            data.speech.byRefFromSpeech = state.speechByRef.load(std::memory_order_relaxed);
            data.speech.byRefFromOther = state.otherByRef.load(std::memory_order_relaxed);
            data.speech.soundsCreated = state.speechCreated.load(std::memory_order_relaxed);
            data.speech.soundsRefused = state.speechRefused.load(std::memory_order_relaxed);
            data.speech.withParams = state.speechWithParams.load(std::memory_order_relaxed);
            data.speech.withWaveSlot = state.speechWithWaveSlot.load(std::memory_order_relaxed);
            data.speech.speechVtables = state.speechVtables.Snapshot();
            data.speech.otherVtables = state.otherVtables.Snapshot();
            data.speech.refs = state.speechRefs.Snapshot();
            data.wetHoof.requests = state.hoofRequests.load(std::memory_order_relaxed);
            data.wetHoof.accepted = state.hoofAccepted.load(std::memory_order_relaxed);
            data.wetHoof.withParams = state.hoofWithParams.load(std::memory_order_relaxed);
            data.wetHoof.withVariables = state.hoofWithVariables.load(std::memory_order_relaxed);
            data.construct.topLevel = state.constructTop.load(std::memory_order_relaxed);
            data.construct.child = state.constructChild.load(std::memory_order_relaxed);
            data.construct.failed = state.constructFailed.load(std::memory_order_relaxed);
            data.construct.topWithSettings = state.constructTopSettings.load(std::memory_order_relaxed);
            data.construct.childWithSettings = state.constructChildSettings.load(std::memory_order_relaxed);
            data.construct.topParentIndexNone = state.constructTopNoParent.load(std::memory_order_relaxed);
            data.construct.childParentIndexNone = state.constructChildNoParent.load(std::memory_order_relaxed);
            data.soundVariables = state.soundVariables.Snapshot();
            data.entityVariables = state.entityVariables.Snapshot();
            data.evolutions = state.evolutions.Snapshot();
            data.globalVariables = state.globalVariables.Snapshot();
            data.globalVariableOverflow = state.globalVariables.Overflow();
            data.fxField5CC = state.fxField5CC.Snapshot();
            data.fxFieldD94 = state.fxFieldD94.Snapshot();
            for (const KeyRow& row : state.fx.Snapshot())
                data.fx.push_back(FxRowOf(row));
            std::sort(data.fx.begin(), data.fx.end(), [](const FxRow& a, const FxRow& b) {
                return a.hash != b.hash ? a.hash < b.hash : a.mode != b.mode ? a.mode < b.mode : a.field < b.field;
            });
            data.fxOverflow = state.fx.Overflow();

            const std::string text = FormatReport(data);
            replay::storage::Folder(kReportFolder); // the base folder (storage_paths.h)
            // Written beside and renamed: a reader never sees half a report.
            const std::wstring temporary = state.reportPath + L".tmp";
            HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return;
            DWORD written = 0;
            const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE &&
                written == text.size();
            CloseHandle(file);
            if (!ok || !MoveFileExW(temporary.c_str(), state.reportPath.c_str(), MOVEFILE_REPLACE_EXISTING))
                DeleteFileW(temporary.c_str());
        }
    }

    bool Enabled()
    {
        ReadIni();
        return g_enabled;
    }

    std::uint64_t Ticks()
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<std::uint64_t>(now.QuadPart);
    }

    bool Install(std::string& summary)
    {
        if (g_state || g_hooksInstalled)
            return true;
        const bool probe = Enabled();
        const bool generic = replay::soundgeneric::Enabled();
        if (!probe && !generic)
        {
            summary = "off";
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
            summary = std::string("mh_initialize_") + MH_StatusToString(init);
            return false;
        }
        g_poolsKnown = true;
        for (const SiteCheck& site : kPoolSites)
            g_poolsKnown = g_poolsKnown && BytesMatch(g_base + site.rva, site.bytes);
        State* state = nullptr;
        std::string name;
        if (probe)
        {
            state = new State();
            state->ownerChanges.reserve(64);
            state->hoofSamples.reserve(32);
            if (!g_poolsKnown)
                state->mismatches += "sound_pools;";
            // The volume port's own first instruction names the write index
            // (read before the hook replaces those bytes).
            std::int32_t displacement = 0;
            const std::uintptr_t volume = g_base + Info(kS18).rva;
            state->writeIndexKnown = BytesMatch(volume, "8B 05") && replay::mem::Copy(&displacement, volume + 2, 4) &&
                volume + 6 + displacement == g_base + kWriteIndexRva;
            if (!state->writeIndexKnown)
                state->mismatches += "write_index;";
            for (const PortInfo& info : kPorts)
                state->status[info.port] = info.access == Access::Tap ? "tap" : info.access == Access::Skipped ? "skipped" : "not_installed";
            SYSTEMTIME local{};
            GetLocalTime(&local);
            state->started = LocalTime(local);
            name = ReportFileName(local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
            state->reportPath = replay::storage::Path(kReportFolder) + L"\\" + std::wstring(name.begin(), name.end());
            state->startTicks = state->lastReport = state->lastExpire = Ticks();
            // The tables exist before the first detour can run.
            g_state = state;
        }
        int installed = 0;
        int wanted = 0;
        std::string off;
        for (const HookSpec& spec : kHooks)
        {
            // With the probe off, only the recorder's ports.
            if (!probe && !spec.generic)
                continue;
            ++wanted;
            const char* status = HookPort(spec);
            if (state)
                state->status[spec.port] = status;
            if (std::strcmp(status, "installed") == 0)
                ++installed;
            else
            {
                off += std::string(Info(spec.port).id) + ":" + status + ";";
                if (state)
                    state->mismatches += std::string(Info(spec.port).id) + ";";
            }
        }
        g_hooksInstalled = true;
        std::string skipped;
        for (const PortInfo& info : kPorts)
            if (info.access == Access::Skipped)
                skipped += std::string(info.id) + ";";
        summary = "ports=" + std::to_string(installed) + "/" + std::to_string(wanted) +
            ",off=" + (off.empty() ? "none" : off) + ",skipped=" + skipped +
            ",pools=" + (g_poolsKnown ? "1" : "0") + ",write_index=" + (state && state->writeIndexKnown ? "1" : "0") +
            ",probe=" + (probe ? "1" : "0") + ",generic=" + (generic ? "1" : "0") +
            ",report=" + (probe ? "ReplayDebug\\" + name : std::string("none"));
        // The taps run even when no detour of the probe could be installed.
        if (probe)
            detail::g_active.store(true, std::memory_order_release);
        return true;
    }

    void Tick(Mode mode)
    {
        State* state = g_state;
        if (!state)
            return;
        REPLAY_PROFILE_ZONE("probe.soundfx_tick");
        const std::uint64_t now = Ticks();
        if (state->lastTick)
            state->modeTicks[CurrentMode() < kModes ? CurrentMode() : 0] += now - state->lastTick;
        state->lastTick = now;
        state->scriptThread = GetCurrentThreadId();
        ++state->frames;
        g_mode.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
        for (PortStats& port : state->ports)
            port.EndFrame();
        if (ToSeconds(now - state->lastExpire) >= 1.0)
        {
            state->lastExpire = now;
            Lock lock(state->lock);
            state->queue.Expire(ToUs(now), kNeverCreatedUs);
        }
        if (ToSeconds(now - state->lastReport) >= kReportEveryS)
        {
            state->lastReport = now;
            WriteReport(*state, false);
        }
    }

    void Shutdown()
    {
        State* state = g_state;
        // The unload disabled every hook and drained every detour before
        // this (module_lifetime.h): no detour and no tap runs any more.
        detail::g_active.store(false, std::memory_order_release);
        if (state)
            WriteReport(*state, true);
        for (std::size_t port = 0; port < kPortCount; ++port)
        {
            if (g_targets[port])
            {
                MH_DisableHook(g_targets[port]);
                MH_RemoveHook(g_targets[port]);
                g_targets[port] = nullptr;
            }
            g_originals[port].store(nullptr, std::memory_order_release);
        }
        g_state = nullptr;
        g_hooksInstalled = false;
        delete state;
    }

    void* OriginalFor(std::uintptr_t rva)
    {
        if (!g_hooksInstalled)
            return nullptr;
        for (const HookSpec& spec : kHooks)
            if (Info(spec.port).rva == rva && g_targets[spec.port])
                return g_originals[spec.port].load(std::memory_order_acquire);
        return nullptr;
    }

    // ---- taps ----

    void NoteCreated(std::uint8_t core, std::uint64_t ref, std::uint64_t audio, std::uint64_t sound,
        const std::uint8_t* params, const std::uint32_t* callers, std::uint32_t callerCount)
    {
        State* state = g_state;
        if (!state)
            return;
        const Port port = core ? kS02 : kS01;
        PortStats& stats = state->ports[port];
        const std::uint32_t thread = GetCurrentThreadId();
        // The census row's frames: the mod's own detour (0, outside the
        // image), then the creator's caller and the path above it.
        std::uint32_t caller = 0;
        bool throughDrain = false;
        for (std::uint32_t i = 0; i < callerCount; ++i)
        {
            if (!caller)
                caller = callers[i];
            throughDrain = throughDrain || (callers[i] >= kDrainBegin && callers[i] < kDrainEnd);
        }
        stats.Note(CurrentMode(), thread, caller);
        state->refs.Add(ref, sound == 0, core ? 2u : 1u);
        float volume = 0.0f;
        std::int16_t pitch = 0;
        std::uint64_t waveSlot = 0;
        if (params)
        {
            std::memcpy(&volume, params + kParamsVolume, 4);
            std::memcpy(&pitch, params + kParamsPitch, 2);
            std::memcpy(&waveSlot, params + kParamsWaveSlot, 8);
            stats.values[0].Add(volume);
            stats.values[1].Add(pitch);
        }
        const bool speech = t_speechByRef > 0;
        if (speech)
        {
            // Probe 6 and "can speech be made again from its ref".
            (sound ? state->speechCreated : state->speechRefused).fetch_add(1, std::memory_order_relaxed);
            if (params)
                state->speechWithParams.fetch_add(1, std::memory_order_relaxed);
            if (waveSlot)
                state->speechWithWaveSlot.fetch_add(1, std::memory_order_relaxed);
            state->speechRefs.Add(ref, sound == 0);
        }
        if (!core)
            (throughDrain ? state->createdThroughDrain : state->createdOther).fetch_add(1, std::memory_order_relaxed);
        std::uint64_t audioNow = audio;
        std::uint64_t trackerNow = 0;
        if (sound)
        {
            ReadHeap(sound + kSoundAudio, audioNow);
            ReadHeap(sound + kSoundTracker, trackerNow);
        }
        const std::uint64_t now = Ticks();
        std::uint64_t waitedUs = 0;
        bool matched = false;
        {
            Lock lock(state->lock);
            // Probe 7: the request the queue's drain is creating now.
            if (!core && throughDrain)
                matched = state->queue.Match(audio, ref, ToUs(now), waitedUs);
            if (!sound)
                ++state->life.refused;
            else
            {
                bool replaced = false;
                LiveSound* live = state->live.Insert(sound, replaced);
                ++state->life.created;
                if (replaced)
                    ++state->life.reusedLive;
                if (!live)
                    ++state->life.tableFull;
                else
                {
                    live->ref = ref;
                    live->audio = audioNow;
                    live->tracker = trackerNow;
                    live->createdUs = ToUs(now);
                    live->core = core;
                    live->speech = speech;
                    live->hasParams = params != nullptr;
                    live->paramVolume = volume;
                    live->paramPitch = pitch;
                }
            }
        }
        if (matched)
            state->ports[kS03].values[0].Add(static_cast<double>(waitedUs) / 1000.0);
        LiveInfo info;
        info.known = sound != 0;
        info.ref = ref;
        info.owner = audioNow;
        AddSample(stats, audio, ref, trackerNow, waveSlot, volume, sound, info, caller, thread);
    }

    void NoteEnqueueRequest(std::uint64_t audio, std::uint64_t ref, std::uint64_t params, void* returnAddress)
    {
        State* state = g_state;
        if (!state)
            return;
        PortStats& stats = state->ports[kS03];
        const std::uint32_t thread = GetCurrentThreadId();
        const std::uint32_t caller = ToRva(returnAddress);
        stats.Note(CurrentMode(), thread, caller);
        state->queueRequests.fetch_add(1, std::memory_order_relaxed);
        LiveInfo info;
        info.ref = ref;
        info.owner = audio;
        AddSample(stats, audio, ref, params, 0, 0.0f, 0, info, caller, thread);
        if (static_cast<std::uint32_t>(ref) != kWetHoofSet)
            return;
        // Probe 8: the wet step layer's request and the variables it carries.
        state->hoofRequests.fetch_add(1, std::memory_order_relaxed);
        std::uint8_t copy[kParamsBytes] = {};
        if (!params || !ReadAny(params, copy))
            return;
        state->hoofWithParams.fetch_add(1, std::memory_order_relaxed);
        WetHoofSample sample;
        sample.ref = ref;
        sample.caller = caller;
        std::memcpy(&sample.variableCount, copy + kParamsVariableCount, 4);
        std::memcpy(sample.words, copy + kParamsVariables, sizeof(sample.words));
        if (sample.variableCount > 0)
            state->hoofWithVariables.fetch_add(1, std::memory_order_relaxed);
        Lock lock(state->lock);
        if (state->hoofSamples.size() >= state->hoofSamples.capacity())
            return;
        for (const WetHoofSample& known : state->hoofSamples)
            if (known.variableCount == sample.variableCount && std::memcmp(known.words, sample.words, sizeof(sample.words)) == 0)
                return;
        state->hoofSamples.push_back(sample);
    }

    void NoteEnqueueResult(std::uint64_t audio, std::uint64_t ref, bool accepted)
    {
        State* state = g_state;
        if (!state)
            return;
        if (!accepted)
        {
            state->queueRefused.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (static_cast<std::uint32_t>(ref) == kWetHoofSet)
            state->hoofAccepted.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t now = ToUs(Ticks());
        Lock lock(state->lock);
        state->queue.Push(audio, ref, now);
    }

    void NoteCall(Port port, void* returnAddress)
    {
        State* state = g_state;
        if (!state || port >= kPortCount)
            return;
        state->ports[port].Note(CurrentMode(), GetCurrentThreadId(), ToRva(returnAddress));
    }

    void NoteForget(std::uint64_t sound, void* returnAddress)
    {
        State* state = g_state;
        if (!state)
            return;
        PortStats& stats = state->ports[kS14];
        const std::uint32_t thread = GetCurrentThreadId();
        const std::uint32_t caller = ToRva(returnAddress);
        stats.Note(CurrentMode(), thread, caller);
        const LiveInfo info = Touch(*state, kS14, sound, caller, false, true);
        AddSample(stats, sound, info.known ? 1 : 0, 0, 0, 0.0f, sound, info, caller, thread);
    }

    void EnterByRef(std::uint64_t audio, std::uint64_t ref, std::uint64_t params, void* returnAddress)
    {
        State* state = g_state;
        if (!state)
            return;
        PortStats& stats = state->ports[kS05];
        const std::uint32_t thread = GetCurrentThreadId();
        const std::uint32_t caller = ToRva(returnAddress);
        stats.Note(CurrentMode(), thread, caller);
        // Probe 6: the two speech sites, and what kind of audio entity they pass.
        const bool speech = caller == kSpeechByRefA || caller == kSpeechByRefB;
        std::uint64_t vtable = 0;
        ReadHeap(audio, vtable);
        (speech ? state->speechVtables : state->otherVtables).Add(ToRva(vtable));
        (speech ? state->speechByRef : state->otherByRef).fetch_add(1, std::memory_order_relaxed);
        if (speech)
            ++t_speechByRef;
        LiveInfo info;
        info.ref = ref;
        info.owner = audio;
        AddSample(stats, audio, ref, params, speech ? 1 : 0, 0.0f, 0, info, caller, thread);
    }

    void LeaveByRef()
    {
        if (t_speechByRef > 0)
            --t_speechByRef;
    }

    // The speech mark alone (the probe off).
    bool EnterSpeechOnly(void* returnAddress) noexcept
    {
        const std::uint32_t caller = ToRva(returnAddress);
        if (caller != kSpeechByRefA && caller != kSpeechByRefB)
            return false;
        ++t_speechByRef;
        return true;
    }

    void LeaveSpeechOnly() noexcept
    {
        if (t_speechByRef > 0)
            --t_speechByRef;
    }

    bool InsideSpeech() noexcept { return t_speechByRef > 0; }

    void NoteRegistered(std::uint64_t startTicks, std::uint64_t engineTicks, std::uint32_t effectHash, void* instance,
        void* returnAddress)
    {
        State* state = g_state;
        if (!state)
            return;
        PortStats& stats = state->ports[kP02];
        const std::uint32_t thread = GetCurrentThreadId();
        const std::uint32_t caller = ToRva(returnAddress);
        const std::size_t mode = CurrentMode();
        stats.Note(mode, thread, caller);
        // Probe 10: the whole detour and the engine's part of it.
        const std::uint64_t now = Ticks();
        stats.values[0].Add(ToSeconds(now - startTicks) * 1000000.0);
        stats.values[1].Add(ToSeconds(engineTicks) * 1000000.0);
        if (!instance || !effectHash)
            return;
        // Probe 11: the instance's state as the game set it by this call.
        const std::uint64_t inst = reinterpret_cast<std::uint64_t>(instance);
        const std::uint8_t modeByte = static_cast<std::uint8_t>(mode);
        float alpha = 0.0f;
        float zoom = 0.0f;
        float colour[3] = {};
        std::uint8_t evolutions[4] = {};
        if (ReadHeap(inst + kInstAlpha, alpha))
            state->fx.AddValue(FxKey(effectHash, modeByte, 0), alpha);
        if (ReadHeap(inst + kInstZoom, zoom))
            state->fx.AddValue(FxKey(effectHash, modeByte, 1), zoom);
        if (ReadHeap(inst + kInstEvolutions, evolutions))
            for (std::uint8_t i = 0; i < 4; ++i)
                state->fx.AddValue(FxKey(effectHash, modeByte, static_cast<std::uint8_t>(2 + i)), evolutions[i]);
        if (ReadHeap(inst + kInstColour, colour))
            for (std::uint8_t i = 0; i < 3; ++i)
                state->fx.AddValue(FxKey(effectHash, modeByte, static_cast<std::uint8_t>(6 + i)), colour[i]);
        AddSample(stats, inst, effectHash, FloatBits(alpha), FloatBits(zoom), alpha, 0, LiveInfo{}, caller, thread);
    }
}
