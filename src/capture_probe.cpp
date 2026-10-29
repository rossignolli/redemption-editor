#include <intrin.h>
#include "capture_probe.h"
#include "render_target.h" // C29 and C30 shared

#include "guarded_memory.h"
#include "hook_batch.h"
#include "mod_identity.h"
#include "module_lifetime.h"
#include "profiler.h"
#include "thread_names.h"
#include "version.generated.h"

#include "main.h"
#include "natives.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// See capture_probe.h. Addresses: build 1491.50; the proof of each is in
// capture_probe_core.h. Poll::Lights sweeps the whole object pool with a
// native budget per poll and counts the lit objects that vanish between two
// complete sweeps (LightTracker, capture_probe_core.h).

namespace replay::captureprobe
{
    namespace
    {
        // ---- engine facts (build 1491.50): the globals the polls read, each
        // behind the instruction that proved it ----

        struct SiteCheck
        {
            const char* name;
            std::uintptr_t rva;
            const char* bytes;
        };

        // Groups: a series reads its globals only when every site of its
        // group matched.
        enum Site : std::uint8_t
        {
            kSiteLightning,
            kSiteTimecycle,
            kSiteWeather,
            kSiteArtificialLights,
            kSiteTv,
            kSiteMapChange,
            kSiteGlass,
            kSiteVehicle,
            kSiteCloth,
            kSiteRayfire,
            kSiteCount,
        };

        struct SiteGroup
        {
            Site site;
            const char* name;
            std::vector<SiteCheck> checks;
        };

        const SiteGroup kSites[] = {
            {kSiteLightning, "lightning_manager",
                {{"manager", 0x733E90, "48 8D 0D 49 A2 7F 04"}, {"type_field", 0x1459C9B, "89 8B 00 6E 00 00"},
                    {"request_field", 0x1459BE0, "40 38 AB E0 6D 00 00"}, {"forced_field", 0x1459C65, "8B 8B E4 6D 00 00"},
                    {"strike_position", 0x14192A8, "0F 11 81 50 6E 00 00"}}},
            {kSiteTimecycle, "timecycle_strength", {{"strength", 0xF8F4FC, "F3 0F 11 05 F0 A0 B0 03"}}},
            {kSiteWeather, "rain_and_snow",
                {{"rain", 0x104373F, "F3 0F 10 05 09 2B 53 03"}, {"snow", 0x104388B, "F3 0F 10 05 C1 29 53 03"},
                    {"rain_override", 0x10477F0, "F3 0F 11 05 48 EC 52 03"}, {"snow_override", 0x1047870, "F3 0F 11 05 CC EB 52 03"}}},
            {kSiteArtificialLights, "artificial_lights", {{"flag", 0xF8E39B, "0F 95 05 19 F3 F7 02"}}},
            {kSiteTv, "tv_channel", {{"channel", 0xF8C797, "8B 05 23 C2 CA 03"}}},
            {kSiteMapChange, "map_change_manager",
                {{"manager", 0xF89B63, "48 8D 0D 36 CF AB 03"}, {"entry_stride", 0xE2D7EE, "48 C1 E7 06 48 03 3B"},
                    {"entry_hashes", 0xE3D4E9, "89 51 04"}}},
            {kSiteGlass, "glass_interface", {{"pointer", 0x6B4018, "48 8B 0D F9 1C 2E 05"}}},
            {kSiteVehicle, "vehicle_fields",
                {{"dirt_mud", 0x15D33A2, "F3 0F 11 84 81 9C 09 00 00"}, {"snow", 0x15D5F06, "F3 0F 11 89 8C 09 00 00"},
                    {"wet", 0x15D7923, "F3 0F 11 81 88 09 00 00"}, {"light", 0x10F4CD3, "80 A7 F0 09 00 00 F8"}}},
            {kSiteCloth, "cloth_tables",
                {{"manager", 0x8DCE95, "48 8B 0D 04 D6 C5 03"}, {"handles", 0x8DCE82, "48 8D 99 C8 04 00 00"},
                    {"ped_index", 0xFEA199, "8B 81 9C 00 00 00"}, {"ped_base", 0xFEA1A4, "2B 05 96 2C A0 02"},
                    {"ped_table", 0xFEA1B1, "48 8B 05 90 2C A0 02"}, {"ped_entry", 0xFEA1B8, "4C 8B 84 01 98 00 00 00"},
                    {"piece_entry", 0x2D1F249, "48 8B 84 01 98 01 00 00"}}},
            {kSiteRayfire, "composite_entity_pool",
                {{"ready", 0x10C595C, "8A 05 A6 8E 97 03"}, {"key", 0x10C596D, "48 8B 0D A4 8E 97 03"},
                    {"pointer", 0x10C5974, "4C 8B 05 95 8E 97 03"}, {"state_field", 0x10C59B0, "8B 80 64 02 00 00"},
                    {"name_field", 0x10C589D, "39 78 60"}, {"pool_fields", 0x2B09388, "48 8B 41 10"}}},
        };

        constexpr std::uintptr_t kLightningManagerRva = 0x4F2E0E0;
        constexpr std::uint64_t kLightningTypeOffset = 0x6E00;
        constexpr std::uint64_t kLightningRequestOffset = 0x6DE0;
        constexpr std::uint64_t kLightningForcedOffset = 0x6DE4;
        constexpr std::uintptr_t kTimecycleStrengthRva = 0x4A995F4;
        constexpr std::uintptr_t kRainRva = 0x4576250;
        constexpr std::uintptr_t kSnowRva = 0x4576254;
        constexpr std::uintptr_t kRainOverrideRva = 0x4576440;
        constexpr std::uintptr_t kSnowOverrideRva = 0x4576444;
        constexpr std::uintptr_t kArtificialLightsRva = 0x3F0D6BB;
        constexpr std::uintptr_t kTvChannelRva = 0x4C389C0;
        constexpr std::uintptr_t kMapChangeManagerRva = 0x4A46AA0; // [+0] entries (0x40 each), u16 count at +8
        constexpr std::uintptr_t kGlassInterfaceRva = 0x5995D18;
        constexpr std::uint64_t kVehicleLightOffset = 0x9F0;
        constexpr std::uint64_t kVehicleDirtOffset = 0x99C;
        constexpr std::uint64_t kVehicleMudOffset = 0x9A0;
        constexpr std::uint64_t kVehicleSnowOffset = 0x98C;
        constexpr std::uint64_t kVehicleWetOffset = 0x988;
        constexpr std::uintptr_t kClothManagerRva = 0x453A4A0; // pointer; [+8] pieces, 0x280 each, piece object at +0x198
        constexpr std::uint64_t kClothHandles = 0x4C8;
        constexpr std::size_t kClothHandleCount = 128;
        constexpr std::uintptr_t kPedVariationBaseRva = 0x39ECE40;
        constexpr std::uintptr_t kPedVariationTableRva = 0x39ECE48; // entries 0x148, object (tagged) at +0x98
        constexpr std::uintptr_t kRayfireReadyRva = 0x4A3E808;
        constexpr std::uintptr_t kRayfirePointerRva = 0x4A3E810;
        constexpr std::uintptr_t kRayfireKeyRva = 0x4A3E818;
        constexpr std::uint64_t kRayfireStateOffset = 0x264;
        constexpr std::uint64_t kRayfireDef = 0x20; // -> +0x60 name hash
        constexpr std::uint32_t kRayfirePoolCap = 2048;
        constexpr int kRayfireAnimatingState = 7;

        // Explosion arguments (0x6A775C).
        constexpr std::uint64_t kExplosionPosition = 0x10;
        constexpr std::uint64_t kExplosionOwner = 0x28;
        // The mixer scene keeps its hash at +0x328 (written by 0x2F19018).
        constexpr std::uint64_t kMixSceneHash = 0x328;
        // The shake metadata's hash.
        constexpr std::uint64_t kShakeMetadataHash = 0x8;

        constexpr wchar_t kReportFolder[] = L"ReplayDebug";
        constexpr double kReportEveryS = 60.0;
        constexpr float kLightRadius = 40.0f;
        constexpr float kVehicleRadius = 30.0f;

        using Fn4 = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        using Fn8 = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t, std::uint64_t);
        using Fn20 = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t);
        using FnF2 = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, float);
        using FnF2x8 = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, float, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t, std::uint64_t);
        using FnF3 = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t, std::uint64_t, float);
        using FnTv = std::uint64_t(__fastcall*)(float, float, float, float, std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t);

        struct State
        {
            PortStats ports[kPortCount];
            std::string status[kPortCount];
            SeriesStats series[kSeriesCount];
            std::string seriesStatus[kSeriesCount];
            bool siteOk[kSiteCount] = {};
            std::vector<SiteRow> sites;
            std::string mismatches;

            // Written by the detours (any thread).
            KeyTable shakes{256};
            KeyTable timecycleLookups{256};
            KeyTable postFxPlay{256};
            KeyTable postFxStop{256};
            KeyTable damageVehicles{256};
            KeyTable deformVehicles{256};
            KeyTable mapChanges{1024};
            KeyTable explosions{128};
            KeyTable glassHandles{1024};
            KeyTable animScenes{256};
            KeyTable mixScenes{512};
            KeyTable mixSceneStops{512};
            KeyTable scriptScenes{256};
            KeyTable sceneVariables{512};
            KeyTable zones{512};
            KeyTable streams{256};
            KeyTable musicEvents{512};
            KeyTable renderTargetLinks{64};
            SRWLOCK lock = SRWLOCK_INIT;
            NameTable names{256}; // under `lock`

            // Script thread only (the polls).
            KeyTable timecycleIndices{256};
            KeyTable pickupModels{256};
            KeyTable lightModels{512};
            KeyTable clothClasses{64};
            KeyTable rayfire{512};
            std::unordered_map<std::uint64_t, int> rayfireLast; // entity -> state
            LightningTracker lightning;
            LightTracker lights;   // probe 26
            LightSweep lightSweep; // the walk in progress over the pool
            std::uint64_t rayfireStateChanges = 0;
            std::uint64_t pollNatives = 0;
            std::uint64_t pollTicks = 0;

            std::uint64_t startTicks = 0;
            std::uint64_t lastTick = 0;
            std::uint64_t lastReport = 0;
            std::uint64_t modeTicks[kModes] = {};
            std::uint64_t frames = 0;
            std::uint32_t scriptThread = 0;
            // Poll clocks.
            std::uint64_t due30 = 0, due10 = 0, due4 = 0, due2 = 0, due1 = 0;
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
        std::atomic<void*> g_originals[kPortCount] = {};
        void* g_targets[kPortCount] = {};
        std::atomic<std::uint8_t> g_mode{0};
        std::uintptr_t g_base = 0;
        std::uintptr_t g_imageEnd = 0;
        LARGE_INTEGER g_frequency{};

        std::uint64_t Ticks()
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            return static_cast<std::uint64_t>(now.QuadPart);
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

        template <typename T>
        bool ReadHeap(std::uint64_t address, T& out)
        {
            return replay::mem::Copy(&out, reinterpret_cast<const void*>(address), sizeof(T), replay::mem::kGameHeapLowest);
        }

        template <typename T>
        bool ReadAny(std::uint64_t address, T& out)
        {
            return replay::mem::Copy(&out, reinterpret_cast<const void*>(address), sizeof(T));
        }

        template <typename T>
        bool ReadGlobal(std::uintptr_t rva, T& out)
        {
            return ReadAny(g_base + rva, out);
        }

        // A C string of the game, byte by byte (a page boundary inside it
        // must not fault the whole read). `out` holds 64 bytes.
        bool ReadCString(std::uint64_t address, char* out)
        {
            for (std::size_t i = 0; i < 63; ++i)
            {
                char c = 0;
                if (!replay::mem::Copy(&c, reinterpret_cast<const void*>(address + i), 1))
                {
                    out[i] = 0;
                    return i > 0;
                }
                out[i] = c;
                if (!c)
                    return true;
            }
            out[63] = 0;
            return true;
        }

        std::size_t CurrentMode() { return g_mode.load(std::memory_order_relaxed); }

        std::uint32_t FloatBits(float value)
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, 4);
            return bits;
        }

        std::uint64_t PackXY(const float* xyz) { return (static_cast<std::uint64_t>(FloatBits(xyz[1])) << 32) | FloatBits(xyz[0]); }

        // What every detour records: the call, and the first distinct ones
        // as a sample.
        void Record(Port port, void* returnAddress, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
            float value)
        {
            State* state = g_state;
            if (!state)
                return;
            PortStats& stats = state->ports[port];
            const std::uint32_t thread = GetCurrentThreadId();
            const std::uint32_t caller = ToRva(returnAddress);
            stats.Note(CurrentMode(), thread, caller);
            if (stats.samples.Full())
                return;
            soundfx::Sample sample;
            sample.args[0] = a0;
            sample.args[1] = a1;
            sample.args[2] = a2;
            sample.args[3] = a3;
            sample.value = value;
            sample.caller = caller;
            sample.thread = thread;
            stats.samples.Add(sample);
        }

        void Value(Port port, int slot, double value)
        {
            if (State* state = g_state)
                state->ports[port].values[slot ? 1 : 0].Add(value);
        }

        void AddName(const char* table, std::uint64_t address)
        {
            State* state = g_state;
            char text[64] = {};
            if (!state || !ReadCString(address, text))
                return;
            Lock lock(state->lock);
            state->names.Add(table, text);
        }

        template <typename Fn>
        Fn Original(Port port)
        {
            return reinterpret_cast<Fn>(g_originals[port].load(std::memory_order_acquire));
        }

        // ---- detours: read, then the original with the same arguments ----

#define CAPTURE_DETOUR_8(NAME, PORT, BODY)                                                                        \
        std::uint64_t __fastcall NAME(std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,        \
            std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7)                                   \
        {                                                                                                             \
            replay::lifetime::DetourScope lifetime; /* the whole call, first */                                       \
            REPLAY_PROFILE_ZONE("detour.capture_probe");                                                              \
            void* const returnAddress = _ReturnAddress();                                                             \
            (void)a4; (void)a5; (void)a6; (void)a7;                                                                   \
            BODY                                                                                                      \
            const Fn8 original = Original<Fn8>(PORT);                                                                 \
            return original ? original(a0, a1, a2, a3, a4, a5, a6, a7) : 0;                                           \
        }

        // The hash a pointer argument points at (0 when unreadable).
        std::uint32_t HashAt(std::uint64_t address)
        {
            std::uint32_t hash = 0;
            return ReadAny(address, hash) ? hash : 0;
        }

        bool Vec3At(std::uint64_t address, float* xyz)
        {
            return ReadAny(address, *reinterpret_cast<float(*)[3]>(xyz));
        }

        // C01 (camera, name, amplitude in xmm2, count, int).
        std::uint64_t __fastcall ShakeByNameDetour(std::uint64_t camera, std::uint64_t name, float amplitude,
            std::uint64_t count, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.capture_probe");
            char text[64] = {};
            const std::uint32_t hash = ReadCString(name, text) ? HashLower(text) : 0;
            Record(kShakeByName, _ReturnAddress(), camera, hash, count, 0, amplitude);
            Value(kShakeByName, 0, amplitude);
            AddName("shake", name);
            const FnF2x8 original = Original<FnF2x8>(kShakeByName);
            return original ? original(camera, name, amplitude, count, a4, a5, a6, a7) : 0;
        }

        // C02 (camera, metadata, amplitude in xmm2, count, int).
        std::uint64_t __fastcall ShakeDetour(std::uint64_t camera, std::uint64_t metadata, float amplitude,
            std::uint64_t count, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.capture_probe");
            const std::uint32_t hash = HashAt(metadata + kShakeMetadataHash);
            Record(kShake, _ReturnAddress(), camera, hash, count, metadata, amplitude);
            Value(kShake, 0, amplitude);
            if (State* state = g_state)
                state->shakes.AddValue(hash, amplitude);
            const FnF2x8 original = Original<FnF2x8>(kShake);
            return original ? original(camera, metadata, amplitude, count, a4, a5, a6, a7) : 0;
        }

        // C03 (manager, position, float in xmm2).
        std::uint64_t __fastcall LightningAtCoordsDetour(std::uint64_t manager, std::uint64_t position, float value)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.capture_probe");
            float xyz[3] = {};
            Vec3At(position, xyz);
            Record(kLightningAtCoords, _ReturnAddress(), manager, PackXY(xyz), FloatBits(xyz[2]), FloatBits(value), value);
            Value(kLightningAtCoords, 0, xyz[0]);
            Value(kLightningAtCoords, 1, xyz[1]);
            const FnF2 original = Original<FnF2>(kLightningAtCoords);
            return original ? original(manager, position, value) : 0;
        }

        // C04 (manager, hash) -> index: recorded after the original.
        std::uint64_t __fastcall TimecycleLookupDetour(std::uint64_t manager, std::uint64_t hash, std::uint64_t a2,
            std::uint64_t a3)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.capture_probe");
            const Fn4 original = Original<Fn4>(kTimecycleLookup);
            const std::uint64_t result = original ? original(manager, hash, a2, a3) : 0;
            const std::uint32_t name = HashAt(hash);
            const std::int32_t index = static_cast<std::int32_t>(result);
            Record(kTimecycleLookup, _ReturnAddress(), manager, name, static_cast<std::uint32_t>(index), 0,
                static_cast<float>(index));
            Value(kTimecycleLookup, 0, index);
            if (State* state = g_state)
                state->timecycleLookups.AddValue(name, index);
            return result;
        }

        CAPTURE_DETOUR_8(PostFxPlayDetour, kPostFxPlay, {
            const std::uint32_t hash = HashAt(a1);
            float duration = 0.0f;
            std::memcpy(&duration, &a4, 4);
            Record(kPostFxPlay, returnAddress, a0, hash, a2, a3, duration);
            if (State* state = g_state)
                state->postFxPlay.AddValue(hash, duration);
        })

        CAPTURE_DETOUR_8(PostFxStopDetour, kPostFxStop, {
            const std::uint32_t hash = HashAt(a1);
            Record(kPostFxStop, returnAddress, a0, hash, a2, 0, 0.0f);
            if (State* state = g_state)
                state->postFxStop.Add(hash);
        })

        // C07: 18 arguments (the damage float is the 6th); 20 are forwarded,
        // the two extra are the caller's own stack and harmless to copy.
        std::uint64_t __fastcall VehicleDamageDetour(std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
            std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7, std::uint64_t a8, std::uint64_t a9,
            std::uint64_t a10, std::uint64_t a11, std::uint64_t a12, std::uint64_t a13, std::uint64_t a14, std::uint64_t a15,
            std::uint64_t a16, std::uint64_t a17, std::uint64_t a18, std::uint64_t a19)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.capture_probe");
            float damage = 0.0f;
            std::memcpy(&damage, &a5, 4);
            Record(kVehicleDamage, _ReturnAddress(), a0, a1, a2, a3, damage);
            Value(kVehicleDamage, 0, damage);
            if (State* state = g_state)
                state->damageVehicles.AddValue(a0 - 0x3F0, damage); // the vehicle (the damage object is vehicle+0x3F0)
            const Fn20 original = Original<Fn20>(kVehicleDamage);
            return original ? original(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19)
                            : 0;
        }

        CAPTURE_DETOUR_8(VehicleDeformDetour, kVehicleDeform, {
            float impulse[3] = {};
            float offset[3] = {};
            Vec3At(a1, impulse);
            Vec3At(a2, offset);
            const double impulseLength = Length3(impulse);
            Record(kVehicleDeform, returnAddress, a0, PackXY(impulse), PackXY(offset), a3, static_cast<float>(impulseLength));
            Value(kVehicleDeform, 0, impulseLength);
            Value(kVehicleDeform, 1, Length3(offset));
            if (State* state = g_state)
                state->deformVehicles.AddValue(a0 - 0x400, impulseLength); // the vehicle (deformation is vehicle+0x400)
        })

        CAPTURE_DETOUR_8(MapChangeDetour, kMapChange, {
            float sphere[4] = {};
            ReadAny(a3, sphere);
            const std::uint32_t type = static_cast<std::uint32_t>(a5);
            Record(kMapChange, returnAddress, (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a2)) << 32) |
                static_cast<std::uint32_t>(a1), PackXY(sphere), FloatBits(sphere[2]), type, sphere[3]);
            Value(kMapChange, 0, type);
            Value(kMapChange, 1, sphere[3]);
            if (State* state = g_state)
                state->mapChanges.Add((static_cast<std::uint64_t>(static_cast<std::uint32_t>(a1)) << 32) |
                    static_cast<std::uint32_t>(a2), false, 1u << (type & 7));
        })

        CAPTURE_DETOUR_8(ExplosionDetour, kExplosion, {
            std::uint32_t tag = 0;
            float xyz[3] = {};
            std::uint64_t owner = 0;
            ReadAny(a0, tag);
            Vec3At(a0 + kExplosionPosition, xyz);
            ReadAny(a0 + kExplosionOwner, owner);
            Record(kExplosion, returnAddress, tag, PackXY(xyz), owner, a3, xyz[2]);
            Value(kExplosion, 0, tag);
            if (State* state = g_state)
                state->explosions.Add(tag, owner == 0);
        })

        // C11 (entity, position, radius in xmm2, impulse, damage, int, byte).
        std::uint64_t __fastcall GlassBreakRadiusDetour(std::uint64_t entity, std::uint64_t position, float radius,
            std::uint64_t impulse, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.capture_probe");
            float xyz[3] = {};
            Vec3At(position, xyz);
            float damage = 0.0f;
            std::memcpy(&damage, &a4, 4);
            Record(kGlassBreakRadius, _ReturnAddress(), entity, PackXY(xyz), FloatBits(xyz[2]), a5, radius);
            Value(kGlassBreakRadius, 0, radius);
            Value(kGlassBreakRadius, 1, damage);
            const FnF2x8 original = Original<FnF2x8>(kGlassBreakRadius);
            return original ? original(entity, position, radius, impulse, a4, a5, a6, a7) : 0;
        }

        CAPTURE_DETOUR_8(GlassHitDetour, kGlassHit, {
            float xyz[3] = {};
            float impulse[3] = {};
            Vec3At(a2, xyz);
            Vec3At(a3, impulse);
            const std::uint32_t handle = static_cast<std::uint16_t>(a0);
            const std::uint32_t crack = static_cast<std::uint32_t>(a1);
            Record(kGlassHit, returnAddress, handle, crack, PackXY(xyz), FloatBits(xyz[2]), static_cast<float>(Length3(impulse)));
            Value(kGlassHit, 0, crack);
            Value(kGlassHit, 1, Length3(impulse));
            if (State* state = g_state)
                state->glassHandles.AddValue(handle, crack);
        })

        CAPTURE_DETOUR_8(ClothPinDetour, kClothPin, {
            std::int32_t handles[kClothHandleCount] = {};
            int pieces = -1;
            if (ReadHeap(a0 + kClothHandles, handles))
                pieces = CountClothHandles(handles, kClothHandleCount);
            Record(kClothPin, returnAddress, a0, static_cast<std::uint8_t>(a1), static_cast<std::uint32_t>(pieces), 0,
                static_cast<float>(pieces));
            if (pieces >= 0)
                Value(kClothPin, 0, pieces);
            Value(kClothPin, 1, static_cast<std::uint8_t>(a1));
        })

        // C14 (dictionary hash, flags, playback list hash, byte) -> handle:
        // recorded after the original.
        std::uint64_t __fastcall AnimSceneCreateDetour(std::uint64_t dictionary, std::uint64_t flags, std::uint64_t list,
            std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.capture_probe");
            const std::uint32_t dictHash = HashAt(dictionary);
            const std::uint32_t flagBits = HashAt(flags);
            const std::uint32_t listHash = HashAt(list);
            const Fn8 original = Original<Fn8>(kAnimSceneCreate);
            const std::uint64_t result = original ? original(dictionary, flags, list, a3, a4, a5, a6, a7) : 0;
            Record(kAnimSceneCreate, _ReturnAddress(), dictHash, listHash, flagBits, static_cast<std::uint32_t>(result), 0.0f);
            if (State* state = g_state)
                state->animScenes.AddValue(dictHash, static_cast<std::int32_t>(result));
            return result;
        }

        CAPTURE_DETOUR_8(MixSceneStartDetour, kMixSceneStart, {
            const std::uint32_t hash = HashAt(a1);
            Record(kMixSceneStart, returnAddress, a0, hash, a2, a3, 0.0f);
            if (State* state = g_state)
                state->mixScenes.Add(hash, a3 != 0, a3 != 0 ? 1u : 2u); // flagged and mask 1: with an owner (the script path)
        })

        CAPTURE_DETOUR_8(MixSceneStopDetour, kMixSceneStop, {
            const std::uint32_t hash = HashAt(a0 + kMixSceneHash);
            Record(kMixSceneStop, returnAddress, a0, hash, static_cast<std::uint8_t>(a1), 0, 0.0f);
            if (State* state = g_state)
                state->mixSceneStops.Add(hash);
        })

        // C17 (script audio, scene hash) -> bool: recorded after the original.
        std::uint64_t __fastcall ScriptSceneStartDetour(std::uint64_t scriptAudio, std::uint64_t hash, std::uint64_t a2,
            std::uint64_t a3)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.capture_probe");
            const Fn4 original = Original<Fn4>(kScriptSceneStart);
            const std::uint64_t result = original ? original(scriptAudio, hash, a2, a3) : 0;
            const bool accepted = static_cast<std::uint8_t>(result) != 0;
            Record(kScriptSceneStart, _ReturnAddress(), scriptAudio, static_cast<std::uint32_t>(hash), accepted ? 1 : 0, 0, 0.0f);
            Value(kScriptSceneStart, 0, accepted ? 1 : 0);
            if (State* state = g_state)
                state->scriptScenes.Add(static_cast<std::uint32_t>(hash), !accepted);
            return result;
        }

        CAPTURE_DETOUR_8(ScriptSceneStopDetour, kScriptSceneStop, {
            Record(kScriptSceneStop, returnAddress, a0, static_cast<std::uint32_t>(a1), 0, 0, 0.0f);
        })

        // C19 (script audio, scene hash, variable hash, value in xmm3).
        std::uint64_t __fastcall SceneVariableDetour(std::uint64_t scriptAudio, std::uint64_t scene, std::uint64_t variable,
            float value)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.capture_probe");
            const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(scene)) << 32) |
                static_cast<std::uint32_t>(variable);
            Record(kSceneVariable, _ReturnAddress(), scriptAudio, key, FloatBits(value), 0, value);
            Value(kSceneVariable, 0, value);
            if (State* state = g_state)
                state->sceneVariables.AddValue(key, value, true);
            const FnF3 original = Original<FnF3>(kSceneVariable);
            return original ? original(scriptAudio, scene, variable, value) : 0;
        }

        CAPTURE_DETOUR_8(AmbientZoneDetour, kAmbientZone, {
            const std::uint32_t zone = static_cast<std::uint32_t>(a1);
            const int enabled = static_cast<std::uint8_t>(a2) ? 1 : 0;
            Record(kAmbientZone, returnAddress, a0, zone, enabled, static_cast<std::uint8_t>(a3), static_cast<float>(enabled));
            Value(kAmbientZone, 0, enabled);
            if (State* state = g_state)
                state->zones.AddValue(zone, enabled, true);
        })

        // C21 (script audio, stream hash, offset, set hash, byte, int) -> bool.
        std::uint64_t __fastcall StreamLoadDetour(std::uint64_t scriptAudio, std::uint64_t stream, std::uint64_t offset,
            std::uint64_t set, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.capture_probe");
            const std::uint32_t streamHash = HashAt(stream);
            const std::uint32_t setHash = HashAt(set);
            const Fn8 original = Original<Fn8>(kStreamLoad);
            const std::uint64_t result = original ? original(scriptAudio, stream, offset, set, a4, a5, a6, a7) : 0;
            const bool accepted = static_cast<std::uint8_t>(result) != 0;
            Record(kStreamLoad, _ReturnAddress(), streamHash, setHash, static_cast<std::uint32_t>(offset), accepted ? 1 : 0,
                0.0f);
            Value(kStreamLoad, 0, accepted ? 1 : 0);
            if (State* state = g_state)
                state->streams.Add(streamHash, !accepted);
            return result;
        }

        CAPTURE_DETOUR_8(StreamPlayPositionDetour, kStreamPlayPosition, {
            float xyz[3] = {};
            Vec3At(a1, xyz);
            Record(kStreamPlayPosition, returnAddress, a0, PackXY(xyz), FloatBits(xyz[2]), static_cast<std::uint32_t>(a2),
                xyz[2]);
            Value(kStreamPlayPosition, 0, xyz[0]);
            Value(kStreamPlayPosition, 1, xyz[1]);
        })

        CAPTURE_DETOUR_8(StreamPlayPedDetour, kStreamPlayPed, {
            Record(kStreamPlayPed, returnAddress, a0, a1, static_cast<std::uint32_t>(a2), 0, 0.0f);
        })

        CAPTURE_DETOUR_8(StreamPlayFrontendDetour, kStreamPlayFrontend, {
            Record(kStreamPlayFrontend, returnAddress, a0, static_cast<std::uint32_t>(a1), 0, 0, 0.0f);
        })

        CAPTURE_DETOUR_8(StreamStopDetour, kStreamStop, {
            Record(kStreamStop, returnAddress, a0, static_cast<std::uint32_t>(a1), 0, 0, 0.0f);
        })

        // C26 (script audio, bank name, byte, vector, float, pointer) -> bool.
        std::uint64_t __fastcall BankRequestDetour(std::uint64_t scriptAudio, std::uint64_t name, std::uint64_t a2,
            std::uint64_t a3, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.capture_probe");
            char text[64] = {};
            const std::uint32_t hash = ReadCString(name, text) ? HashLower(text) : 0;
            const Fn8 original = Original<Fn8>(kBankRequest);
            const std::uint64_t result = original ? original(scriptAudio, name, a2, a3, a4, a5, a6, a7) : 0;
            const bool accepted = static_cast<std::uint8_t>(result) != 0;
            Record(kBankRequest, _ReturnAddress(), scriptAudio, hash, static_cast<std::uint8_t>(a2), accepted ? 1 : 0, 0.0f);
            Value(kBankRequest, 0, accepted ? 1 : 0);
            AddName("bank", name);
            return result;
        }

        CAPTURE_DETOUR_8(MusicEventDetour, kMusicEvent, {
            const std::uint32_t hash = HashAt(a1);
            const std::uint32_t caller = ToRva(returnAddress);
            // The two native bodies are 0xF79250 and 0xF7929C; the rest is the engine's music code.
            const bool fromScript = caller >= 0xF79250 && caller < 0xF792D2;
            Record(kMusicEvent, returnAddress, a0, hash, a2, static_cast<std::uint32_t>(a3), fromScript ? 1.0f : 0.0f);
            Value(kMusicEvent, 0, fromScript ? 1 : 0);
            if (State* state = g_state)
                state->musicEvents.Add(hash, fromScript, fromScript ? 1u : 2u);
        })

        // C28 (x, y, width, height in xmm0..xmm3, rotation, r, g, b, a).
        std::uint64_t __fastcall TvDrawDetour(float x, float y, float width, float height, std::uint64_t rotation,
            std::uint64_t red, std::uint64_t green, std::uint64_t blue, std::uint64_t alpha)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.capture_probe");
            Record(kTvDraw, _ReturnAddress(), (static_cast<std::uint64_t>(FloatBits(y)) << 32) | FloatBits(x),
                (static_cast<std::uint64_t>(FloatBits(height)) << 32) | FloatBits(width), rotation & 0xFFFFFFFFu,
                (red & 0xFF) | ((green & 0xFF) << 8) | ((blue & 0xFF) << 16) | ((alpha & 0xFF) << 24), width);
            Value(kTvDraw, 0, width);
            Value(kTvDraw, 1, height);
            const FnTv original = Original<FnTv>(kTvDraw);
            return original ? original(x, y, width, height, rotation, red, green, blue, alpha) : 0;
        }

        // C29 (name, bool) -> bool.
        std::uint64_t __fastcall RenderTargetRegisterDetour(std::uint64_t name, std::uint64_t flag, std::uint64_t a2,
            std::uint64_t a3)
        {
            replay::lifetime::DetourScope lifetime; // the whole call, first
            REPLAY_PROFILE_ZONE("detour.capture_probe");
            char text[64] = {};
            const std::uint32_t hash = ReadCString(name, text) ? HashLower(text) : 0;
            const Fn4 original = Original<Fn4>(kRenderTargetRegister);
            const std::uint64_t result = original ? original(name, flag, a2, a3) : 0;
            const bool accepted = static_cast<std::uint8_t>(result) != 0;
            Record(kRenderTargetRegister, _ReturnAddress(), hash, static_cast<std::uint8_t>(flag), accepted ? 1 : 0, 0, 0.0f);
            Value(kRenderTargetRegister, 0, accepted ? 1 : 0);
            AddName("render_target", name);
            return result;
        }

        CAPTURE_DETOUR_8(RenderTargetLinkDetour, kRenderTargetLink, {
            const std::uint32_t model = static_cast<std::uint32_t>(a0);
            Record(kRenderTargetLink, returnAddress, model, 0, 0, 0, 0.0f);
            if (State* state = g_state)
                state->renderTargetLinks.Add(model);
        })

        // ---- install ----

        struct HookSpec
        {
            Port port;
            void* detour;
        };

        const HookSpec kHooks[] = {
            {kShakeByName, reinterpret_cast<void*>(&ShakeByNameDetour)},
            {kShake, reinterpret_cast<void*>(&ShakeDetour)},
            {kLightningAtCoords, reinterpret_cast<void*>(&LightningAtCoordsDetour)},
            {kTimecycleLookup, reinterpret_cast<void*>(&TimecycleLookupDetour)},
            {kPostFxPlay, reinterpret_cast<void*>(&PostFxPlayDetour)},
            {kPostFxStop, reinterpret_cast<void*>(&PostFxStopDetour)},
            {kVehicleDamage, reinterpret_cast<void*>(&VehicleDamageDetour)},
            {kVehicleDeform, reinterpret_cast<void*>(&VehicleDeformDetour)},
            {kMapChange, reinterpret_cast<void*>(&MapChangeDetour)},
            {kExplosion, reinterpret_cast<void*>(&ExplosionDetour)},
            {kGlassBreakRadius, reinterpret_cast<void*>(&GlassBreakRadiusDetour)},
            {kGlassHit, reinterpret_cast<void*>(&GlassHitDetour)},
            {kClothPin, reinterpret_cast<void*>(&ClothPinDetour)},
            {kAnimSceneCreate, reinterpret_cast<void*>(&AnimSceneCreateDetour)},
            {kMixSceneStart, reinterpret_cast<void*>(&MixSceneStartDetour)},
            {kMixSceneStop, reinterpret_cast<void*>(&MixSceneStopDetour)},
            {kScriptSceneStart, reinterpret_cast<void*>(&ScriptSceneStartDetour)},
            {kScriptSceneStop, reinterpret_cast<void*>(&ScriptSceneStopDetour)},
            {kSceneVariable, reinterpret_cast<void*>(&SceneVariableDetour)},
            {kAmbientZone, reinterpret_cast<void*>(&AmbientZoneDetour)},
            {kStreamLoad, reinterpret_cast<void*>(&StreamLoadDetour)},
            {kStreamPlayPosition, reinterpret_cast<void*>(&StreamPlayPositionDetour)},
            {kStreamPlayPed, reinterpret_cast<void*>(&StreamPlayPedDetour)},
            {kStreamPlayFrontend, reinterpret_cast<void*>(&StreamPlayFrontendDetour)},
            {kStreamStop, reinterpret_cast<void*>(&StreamStopDetour)},
            {kBankRequest, reinterpret_cast<void*>(&BankRequestDetour)},
            {kMusicEvent, reinterpret_cast<void*>(&MusicEventDetour)},
            {kTvDraw, reinterpret_cast<void*>(&TvDrawDetour)},
            {kRenderTargetRegister, reinterpret_cast<void*>(&RenderTargetRegisterDetour)},
            {kRenderTargetLink, reinterpret_cast<void*>(&RenderTargetLinkDetour)},
        };
        static_assert(sizeof(kHooks) / sizeof(kHooks[0]) == kPortCount, "one detour per port");

        bool BytesMatch(std::uintptr_t address, const char* text)
        {
            const soundfx::Signature signature = soundfx::ParseSignature(text);
            if (!signature.valid || signature.bytes.size() > 96)
                return false;
            std::uint8_t code[96] = {};
            return replay::mem::Copy(code, address, signature.bytes.size()) && soundfx::MatchSignature(code, signature);
        }

        const char* HookPort(const HookSpec& spec)
        {
            // C29 and C30 ride the render target recorder's
            // detours (NoteRenderTargetRegister/Link below).
            if ((spec.port == kRenderTargetRegister || spec.port == kRenderTargetLink) &&
                replay::rendertarget::OwnsProbePorts())
                return "shared_render_target";
            const PortInfo& info = Info(spec.port);
            const std::uintptr_t address = g_base + info.rva;
            if (!BytesMatch(address, info.signature))
                return "signature_mismatch";
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
            g_enabled = replay::identity::IniInt("CaptureProbe", 0, ini.c_str()) != 0;
        }

        std::string LocalTime(const SYSTEMTIME& time)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d", time.wYear, time.wMonth, time.wDay, time.wHour,
                time.wMinute, time.wSecond);
            return text;
        }

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

        // Which site group a series needs (none: natives only).
        int SiteOf(Series series)
        {
            switch (series)
            {
            case kRainGlobal: case kSnowGlobal: case kRainOverride: case kSnowOverride: return kSiteWeather;
            case kLightningType: case kLightningRequest: case kLightningForcedType: return kSiteLightning;
            case kTimecycleStrength: return kSiteTimecycle;
            case kVehicleLightState: case kVehicleDirt: case kVehicleMud: case kVehicleSnow: case kVehicleWet: return kSiteVehicle;
            case kMapChangesActive: return kSiteMapChange;
            case kGlassInterface: return kSiteGlass;
            case kPlayerClothPieces: case kMountClothPieces: return kSiteCloth;
            case kArtificialLights: return kSiteArtificialLights;
            case kRayfireLive: case kRayfireAnimating: return kSiteRayfire;
            case kTvChannel: return kSiteTv;
            default: return -1;
            }
        }

        void WriteReport(State& state, bool final)
        {
            REPLAY_PROFILE_ZONE("probe.capture_report");
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
            data.sites = state.sites;
            std::vector<std::uint32_t> threads;
            for (const PortInfo& info : kPorts)
            {
                PortRow row;
                row.port = info.port;
                row.data = soundfx::Snapshot(soundfx::kS01, state.ports[info.port], state.status[info.port]);
                for (const std::uint32_t id : row.data.threads)
                    if (std::find(threads.begin(), threads.end(), id) == threads.end())
                        threads.push_back(id);
                data.ports.push_back(std::move(row));
            }
            std::sort(threads.begin(), threads.end());
            for (const std::uint32_t id : threads)
                data.threads.push_back({id, ThreadNameOf(state, id)});
            for (const SeriesInfo& info : kSeries)
            {
                SeriesRow row;
                row.series = info.series;
                row.status = state.seriesStatus[info.series];
                row.stats = state.series[info.series];
                data.series.push_back(std::move(row));
            }
            const auto table = [&data](const char* name, const KeyTable& keys) {
                KeyTableRows rows;
                rows.name = name;
                rows.rows = keys.Snapshot();
                rows.overflow = keys.Overflow();
                data.tables.push_back(std::move(rows));
            };
            table("shakes", state.shakes);
            table("timecycle_lookups", state.timecycleLookups);
            table("timecycle_indices", state.timecycleIndices);
            table("animpostfx_play", state.postFxPlay);
            table("animpostfx_stop", state.postFxStop);
            table("damage_vehicles", state.damageVehicles);
            table("deform_vehicles", state.deformVehicles);
            table("map_changes", state.mapChanges);
            table("explosions", state.explosions);
            table("glass_handles", state.glassHandles);
            table("cloth_classes", state.clothClasses);
            table("anim_scenes", state.animScenes);
            table("mixer_scenes", state.mixScenes);
            table("mixer_scene_stops", state.mixSceneStops);
            table("script_scenes", state.scriptScenes);
            table("scene_variables", state.sceneVariables);
            table("ambient_zones", state.zones);
            table("streams", state.streams);
            table("music_events", state.musicEvents);
            table("render_target_links", state.renderTargetLinks);
            table("pickup_models", state.pickupModels);
            table("light_models", state.lightModels);
            table("rayfire", state.rayfire);
            {
                Lock lock(state.lock);
                data.names = state.names.Rows();
                data.nameOverflow = state.names.Overflow();
            }
            data.lightning = state.lightning;
            data.lights = state.lights;
            data.rayfireStateChanges = state.rayfireStateChanges;
            data.pollNatives = state.pollNatives;
            data.pollMs = ToSeconds(state.pollTicks) * 1000.0;

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

        // ---- polls (script thread) ----

        struct Poll
        {
            State& state;
            std::size_t mode;
            Ped player = 0;
            Vector3 playerPos{};
            bool playerKnown = false;

            void Add(Series series, double value, double epsilon = 1e-4)
            {
                state.series[series].Add(mode, value, epsilon);
            }
            void Unread(Series series) { state.series[series].Unread(); }
            void Native(int count = 1) { state.pollNatives += count; }

            bool Site(Site site) const { return state.siteOk[site]; }

            // Series that only read globals are not sampled when their site
            // check failed (the status already says why).
            template <typename T>
            bool Global(Series series, std::uintptr_t rva, T& out)
            {
                if (!ReadGlobal(rva, out))
                {
                    Unread(series);
                    return false;
                }
                return true;
            }

            void FindPlayer()
            {
                player = PLAYER::PLAYER_PED_ID();
                Native(2);
                playerKnown = player != 0 && ENTITY::DOES_ENTITY_EXIST(player);
                if (playerKnown)
                {
                    playerPos = ENTITY::GET_ENTITY_COORDS(player, TRUE, TRUE);
                    Native();
                }
            }

            float DistanceTo(const Vector3& at) const
            {
                const float dx = at.x - playerPos.x;
                const float dy = at.y - playerPos.y;
                const float dz = at.z - playerPos.z;
                return std::sqrt(dx * dx + dy * dy + dz * dz);
            }

            // Every frame: probe 16.
            void Lightning()
            {
                if (!Site(kSiteLightning))
                    return;
                const std::uint64_t manager = g_base + kLightningManagerRva;
                std::int32_t type = 0;
                std::uint8_t request = 0;
                std::int32_t forced = 0;
                if (!ReadAny(manager + kLightningTypeOffset, type) || !ReadAny(manager + kLightningRequestOffset, request) ||
                    !ReadAny(manager + kLightningForcedOffset, forced))
                {
                    Unread(kLightningType);
                    return;
                }
                Add(kLightningType, type);
                Add(kLightningRequest, request ? 1 : 0);
                Add(kLightningForcedType, forced);
                state.lightning.Step(type, request != 0, forced);
            }

            // 30 Hz: probe 15.
            void Weather()
            {
                Add(kWindSpeed, MISC::GET_WIND_SPEED(), 0.01);
                const Vector3 wind = MISC::GET_WIND_DIRECTION();
                Add(kWindDirX, wind.x, 0.01);
                Add(kWindDirY, wind.y, 0.01);
                Add(kRainNative, MISC::GET_RAIN_LEVEL(), 0.005);
                Add(kSnowNative, MISC::GET_SNOW_LEVEL(), 0.005);
                Native(4);
                if (Site(kSiteWeather))
                {
                    float value = 0.0f;
                    if (Global(kRainGlobal, kRainRva, value))
                        Add(kRainGlobal, value, 0.005);
                    if (Global(kSnowGlobal, kSnowRva, value))
                        Add(kSnowGlobal, value, 0.005);
                    if (Global(kRainOverride, kRainOverrideRva, value))
                        Add(kRainOverride, value, 0.005);
                    if (Global(kSnowOverride, kSnowOverrideRva, value))
                        Add(kSnowOverride, value, 0.005);
                }
            }

            // 10 Hz: probes 14, 17, 27.
            void Camera()
            {
                const float finalFov = CAM::GET_FINAL_RENDERED_CAM_FOV();
                const float gameplayFov = CAM::GET_GAMEPLAY_CAM_FOV();
                Add(kFinalFov, finalFov, 0.05);
                Add(kGameplayFov, gameplayFov, 0.05);
                Add(kFovDelta, finalFov - gameplayFov, 0.05);
                Add(kCamShaking, CAM::IS_GAMEPLAY_CAM_SHAKING() ? 1 : 0);
                Add(kAimCam, CAM::IS_AIM_CAM_ACTIVE() ? 1 : 0);
                Add(kFirstPerson, (CAM::_IS_IN_FULL_FIRST_PERSON_MODE() || CAM::IS_FIRST_PERSON_AIM_CAM_ACTIVE()) ? 1 : 0);
                Add(kCinematicCam, CAM::IS_CINEMATIC_CAM_RENDERING() ? 1 : 0);
                Add(kCinematicMode, CAM::_IS_IN_CINEMATIC_MODE() ? 1 : 0);
                Add(kScreenFading, (CAM::IS_SCREEN_FADING_OUT() || CAM::IS_SCREEN_FADING_IN()) ? 1 : 0);
                Add(kScreenFadedOut, CAM::IS_SCREEN_FADED_OUT() ? 1 : 0);
                Native(11);
                // 27
                Add(kAnimSceneCam, CAM::_IS_ANIM_SCENE_CAM_ACTIVE() ? 1 : 0);
                Add(kScriptCam, CAM::IS_GAMEPLAY_CAM_RENDERING() ? 0 : 1);
                Add(kLetterbox, CAM::HAS_LETTER_BOX() ? 1 : 0);
                Add(kControlOff, PLAYER::IS_PLAYER_CONTROL_ON(PLAYER::PLAYER_ID()) ? 0 : 1);
                Native(5);
                if (playerKnown)
                {
                    Add(kPlayerInAnimScene, ANIMSCENE::IS_ENTITY_PLAYING_ANIM_SCENE(player, -1) ? 1 : 0);
                    Native();
                }
            }

            void Timecycle()
            {
                const int index = GRAPHICS::GET_TIMECYCLE_MODIFIER_INDEX();
                const int transition = GRAPHICS::GET_TIMECYCLE_TRANSITION_MODIFIER_INDEX();
                Native(2);
                Add(kTimecycleIndex, index);
                Add(kTimecycleTransitionIndex, transition);
                state.timecycleIndices.Add(static_cast<std::uint32_t>(index));
                if (Site(kSiteTimecycle))
                {
                    float strength = 0.0f;
                    if (Global(kTimecycleStrength, kTimecycleStrengthRva, strength))
                        Add(kTimecycleStrength, strength, 0.005);
                }
            }

            // 4 Hz: probes 18 and 19.
            Vehicle TrackedVehicle()
            {
                if (!playerKnown)
                    return 0;
                Vehicle vehicle = PED::GET_VEHICLE_PED_IS_IN(player, TRUE);
                Native();
                if (vehicle && ENTITY::DOES_ENTITY_EXIST(vehicle))
                {
                    Native();
                    return vehicle;
                }
                int handles[64] = {};
                const int count = worldGetAllVehicles(handles, 64);
                Vehicle nearest = 0;
                float best = kVehicleRadius;
                for (int i = 0; i < count; ++i)
                {
                    const Vector3 at = ENTITY::GET_ENTITY_COORDS(handles[i], FALSE, TRUE);
                    Native();
                    const float distance = DistanceTo(at);
                    if (distance < best)
                    {
                        best = distance;
                        nearest = handles[i];
                    }
                }
                return nearest;
            }

            void WaterAndVehicle()
            {
                if (!playerKnown)
                    return;
                Add(kPlayerSubmerged, ENTITY::GET_ENTITY_SUBMERGED_LEVEL(player), 0.01);
                Native();
                if (PED::IS_PED_ON_MOUNT(player))
                {
                    const Ped mount = PED::GET_MOUNT(player);
                    Native();
                    if (mount)
                    {
                        Add(kMountSubmerged, ENTITY::GET_ENTITY_SUBMERGED_LEVEL(mount), 0.01);
                        Native();
                    }
                }
                Native();
                float withWaves = 0.0f;
                float without = 0.0f;
                const bool a = WATER::GET_WATER_HEIGHT(playerPos.x, playerPos.y, playerPos.z, &withWaves);
                const bool b = WATER::GET_WATER_HEIGHT_NO_WAVES(playerPos.x, playerPos.y, playerPos.z, &without);
                Native(2);
                if (a && b)
                    Add(kWaveHeight, withWaves - without, 0.005);
                else
                    Unread(kWaveHeight);

                const Vehicle vehicle = TrackedVehicle();
                if (!vehicle)
                    return;
                Add(kVehicleSubmerged, ENTITY::GET_ENTITY_SUBMERGED_LEVEL(vehicle), 0.01);
                Add(kVehicleSpeed, ENTITY::GET_ENTITY_SPEED(vehicle), 0.05);
                Native(2);
                if (!Site(kSiteVehicle))
                    return;
                const std::uint64_t address = reinterpret_cast<std::uint64_t>(getScriptHandleBaseAddress(vehicle));
                std::uint8_t light = 0;
                float dirt = 0.0f, mud = 0.0f, snow = 0.0f, wet = 0.0f;
                if (!address || !ReadHeap(address + kVehicleLightOffset, light) || !ReadHeap(address + kVehicleDirtOffset, dirt) ||
                    !ReadHeap(address + kVehicleMudOffset, mud) || !ReadHeap(address + kVehicleSnowOffset, snow) ||
                    !ReadHeap(address + kVehicleWetOffset, wet))
                {
                    Unread(kVehicleLightState);
                    return;
                }
                Add(kVehicleLightState, VehicleLightState(light));
                Add(kVehicleDirt, dirt, 0.005);
                Add(kVehicleMud, mud, 0.005);
                Add(kVehicleSnow, snow, 0.005);
                Add(kVehicleWet, wet, 0.005);
            }

            // 2 Hz: probes 21 and 29.
            void MapChanges()
            {
                if (!Site(kSiteMapChange))
                    return;
                std::uint16_t count = 0;
                if (Global(kMapChangesActive, kMapChangeManagerRva + 8, count))
                    Add(kMapChangesActive, count);
            }

            void Rayfire()
            {
                if (!Site(kSiteRayfire))
                    return;
                std::uint8_t ready = 0;
                std::uint64_t scrambled = 0;
                std::uint64_t key = 0;
                if (!Global(kRayfireLive, kRayfireReadyRva, ready) || !Global(kRayfireLive, kRayfirePointerRva, scrambled) ||
                    !Global(kRayfireLive, kRayfireKeyRva, key))
                    return;
                const std::uint64_t pool = DecodePoolPointer(ready, scrambled, key);
                if (!pool)
                {
                    Add(kRayfireLive, 0);
                    Add(kRayfireAnimating, 0);
                    return;
                }
                std::uint64_t storage = 0;
                std::uint64_t flags = 0;
                std::uint32_t size = 0;
                std::uint32_t stride = 0;
                if (!ReadHeap(pool + 0x08, storage) || !ReadHeap(pool + 0x10, flags) || !ReadHeap(pool + 0x18, size) ||
                    !ReadHeap(pool + 0x1C, stride) || !storage || !flags || !stride || size > kRayfirePoolCap)
                {
                    Unread(kRayfireLive);
                    return;
                }
                int live = 0;
                int animating = 0;
                for (std::uint32_t i = 0; i < size; ++i)
                {
                    std::uint8_t flag = 0;
                    if (!ReadHeap(flags + i, flag) || !PoolSlotLive(flag))
                        continue;
                    const std::uint64_t entity = storage + static_cast<std::uint64_t>(i) * stride;
                    std::int32_t stateValue = 0;
                    std::uint64_t def = 0;
                    std::uint32_t name = 0;
                    if (!ReadHeap(entity + kRayfireStateOffset, stateValue))
                        continue;
                    ++live;
                    if (stateValue == kRayfireAnimatingState)
                        ++animating;
                    if (ReadHeap(entity + kRayfireDef, def) && def)
                        ReadHeap(def + 0x60, name);
                    state.rayfire.AddValue(name ? name : static_cast<std::uint32_t>(i), stateValue);
                    const auto known = state.rayfireLast.find(entity);
                    if (known != state.rayfireLast.end() && known->second != stateValue)
                        ++state.rayfireStateChanges;
                    state.rayfireLast[entity] = stateValue;
                }
                Add(kRayfireLive, live);
                Add(kRayfireAnimating, animating);
            }

            // 1 Hz: probes 22, 24, 25, 26, 30.
            void Pickups()
            {
                int handles[256] = {};
                const int count = worldGetAllPickups(handles, 256);
                int withObject = 0;
                for (int i = 0; i < count; ++i)
                {
                    const Object object = OBJECT::GET_PICKUP_OBJECT(handles[i]);
                    Native();
                    if (!object || !ENTITY::DOES_ENTITY_EXIST(object))
                        continue;
                    ++withObject;
                    state.pickupModels.Add(ENTITY::GET_ENTITY_MODEL(object));
                    Native(2);
                }
                Add(kPickupCount, count);
                Add(kPickupObjects, withObject);
            }

            void GlassInterface()
            {
                if (!Site(kSiteGlass))
                    return;
                std::uint64_t pointer = 0;
                if (Global(kGlassInterface, kGlassInterfaceRva, pointer))
                    Add(kGlassInterface, pointer ? 1 : 0);
            }

            // The variation object of a ped (SET_PED_CLOTH_PIN_FRAMES's path)
            // and its cloth pieces; -1 when a step cannot be read.
            int ClothPieces(Ped ped)
            {
                const std::uint64_t address = reinterpret_cast<std::uint64_t>(getScriptHandleBaseAddress(ped));
                std::uint32_t index = 0;
                std::int32_t base = 0;
                std::uint64_t table = 0;
                std::uint64_t tagged = 0;
                if (!address || !ReadHeap(address + 0x9C, index) || !ReadGlobal(kPedVariationBaseRva, base) ||
                    !ReadGlobal(kPedVariationTableRva, table) || !table)
                    return -1;
                const std::int64_t slot = static_cast<std::int64_t>(index & 0x1FFFF) - base;
                if (slot < 0 || slot > 0x20000)
                    return -1;
                if (!ReadHeap(table + static_cast<std::uint64_t>(slot) * 0x148 + 0x98, tagged))
                    return -1;
                const std::uint64_t object = tagged & ~1ull;
                if (!object)
                    return 0;
                std::int32_t handles[kClothHandleCount] = {};
                if (!ReadHeap(object + kClothHandles, handles))
                    return -1;
                const int pieces = CountClothHandles(handles, kClothHandleCount);
                // The class of each piece (the lookup 0x2D1F230 reads).
                std::uint64_t manager = 0;
                std::uint64_t array = 0;
                if (pieces && ReadGlobal(kClothManagerRva, manager) && manager && ReadHeap(manager + 8, array) && array)
                {
                    for (std::size_t i = 0; i < kClothHandleCount; ++i)
                    {
                        if (handles[i] == -1 || handles[i] < 0)
                            continue;
                        std::uint64_t piece = 0;
                        std::uint64_t vtable = 0;
                        if (ReadHeap(array + static_cast<std::uint64_t>(handles[i]) * 0x280 + 0x198, piece) && piece &&
                            ReadHeap(piece, vtable))
                            state.clothClasses.Add(ToRva(vtable));
                    }
                }
                return pieces;
            }

            void Cloth()
            {
                if (!Site(kSiteCloth) || !playerKnown)
                    return;
                const int pieces = ClothPieces(player);
                if (pieces < 0)
                    Unread(kPlayerClothPieces);
                else
                    Add(kPlayerClothPieces, pieces);
                if (PED::IS_PED_ON_MOUNT(player))
                {
                    const Ped mount = PED::GET_MOUNT(player);
                    Native();
                    if (mount)
                    {
                        const int mountPieces = ClothPieces(mount);
                        if (mountPieces < 0)
                            Unread(kMountClothPieces);
                        else
                            Add(kMountClothPieces, mountPieces);
                    }
                }
                Native();
            }

            // 1 Hz: probe 26. The whole pool, LightSweep::kNativeBudget
            // natives per poll: a sweep that does not fit resumes in the next
            // poll where it stopped, and the series are per complete sweep.
            // A lit object the next complete sweep does not see is counted as
            // vanished (a shot lamp becomes a fragment object with a new
            // handle, so the old one never "goes dark"); the first 32 are
            // sampled with DOES_ENTITY_EXIST, the distance when it still
            // exists (it left the radius) and the model of the nearest object
            // that appeared within 2 m in the same sweep (the fragment).
            // Cost: pool count + 2 per object within 40 m + 1 per lit object
            // per sweep, and 1 to 3 per vanished object at the sweep's end;
            // light_sweep_natives and light_sweep_polls measure it.
            //
            // Candidate port for the break itself (not installed: see the
            // note at the end of this function).
            void Lights()
            {
                if (Site(kSiteArtificialLights))
                {
                    std::uint8_t off = 0;
                    if (Global(kArtificialLights, kArtificialLightsRva, off))
                        Add(kArtificialLights, off ? 1 : 0);
                }
                if (!playerKnown)
                    return;
                LightSweep& sweep = state.lightSweep;
                LightTracker& lights = state.lights;
                if (!sweep.active)
                    sweep.Begin(worldGetAllObjects(sweep.handles, LightSweep::kCapacity));
                sweep.NewPoll();
                while (sweep.Walking() && sweep.HasBudget())
                {
                    const Object handle = sweep.handles[sweep.cursor++];
                    const Vector3 at = ENTITY::GET_ENTITY_COORDS(handle, FALSE, TRUE);
                    sweep.Spend(1);
                    if (DistanceTo(at) > kLightRadius)
                        continue;
                    ++sweep.checked;
                    LightSeen object;
                    object.handle = handle;
                    object.xyz[0] = at.x;
                    object.xyz[1] = at.y;
                    object.xyz[2] = at.z;
                    object.intensity = OBJECT::_GET_OBJECT_LIGHT_INTENSITY(handle);
                    sweep.Spend(1);
                    if (object.intensity > 0.0f)
                    {
                        ++sweep.lit;
                        object.model = ENTITY::GET_ENTITY_MODEL(handle);
                        sweep.Spend(1);
                        state.lightModels.AddValue(object.model, object.intensity);
                    }
                    lights.Note(object);
                }
                if (sweep.Done())
                {
                    for (const LightSeen& old : lights.Vanished())
                    {
                        const bool exists = ENTITY::DOES_ENTITY_EXIST(old.handle) != FALSE;
                        sweep.Spend(1);
                        float distance = -1.0f;
                        if (exists)
                        {
                            distance = DistanceTo(ENTITY::GET_ENTITY_COORDS(old.handle, FALSE, TRUE));
                            sweep.Spend(1);
                        }
                        if (!lights.Vanish(old, exists))
                            continue;
                        LitVanishedSample sample;
                        sample.model = old.model;
                        std::memcpy(sample.xyz, old.xyz, sizeof(sample.xyz));
                        sample.intensity = old.intensity;
                        sample.exists = exists;
                        sample.distance = distance;
                        float newDistance = -1.0f;
                        if (const int fresh = lights.NearestNew(old.xyz, newDistance))
                        {
                            sample.newModel = ENTITY::GET_ENTITY_MODEL(fresh);
                            sweep.Spend(1);
                            sample.newDistance = newDistance;
                        }
                        lights.AddSample(sample);
                    }
                    lights.EndSweep();
                    Add(kLightsPool, sweep.count);
                    Add(kLightsNear, sweep.checked);
                    Add(kLightsLit, sweep.lit);
                    Add(kLightSweepNatives, sweep.natives);
                    Add(kLightSweepPolls, sweep.polls);
                    sweep.End();
                }
                Native(sweep.pollNatives);
                // Candidate ports for the break itself (not installed):
                //   0x6E38AC (delegate registered at 0x6D973F, no direct
                //   caller) creates the object of a broken fragment and sets
                //   bit 0 of [new+0xB0] (the bit HAS_OBJECT_BEEN_BROKEN
                //   tests) on the new object and its parent; whether it runs
                //   on every break is not proved.
                //   0xE482D4 (called from the impulse break 0x6F5B98, slot
                //   71 of the fragment instance vtable 0x33CBDB8) plays the
                //   break effects and, through 0x12A0B20 -> 0x633CB4, sets
                //   bit 13 of [light+0xB0] for each child light: the light
                //   goes out by state query in its update, not by an event.
                //   Not every kind of break goes through it (the parts
                //   broken off override 0x6EAE68, slot 87, uses 0xE47B50).
                // The per-object intensity _SET_LIGHT_INTENSITY_FOR_OBJECT
                // writes (object+0x734, 0x10CF2A2) is not touched by the
                // break path: _GET_OBJECT_LIGHT_INTENSITY (0x10C6184) reads
                // the model's light attribute, not that field, which is why
                // a broken lamp never reads as "dark" and only its handle
                // vanishing (counted above) shows the break.
            }

            void Tv()
            {
                if (!Site(kSiteTv))
                    return;
                std::int32_t channel = 0;
                if (Global(kTvChannel, kTvChannelRva, channel))
                    Add(kTvChannel, channel);
            }
        };

        bool Due(std::uint64_t& last, std::uint64_t now, double periodS)
        {
            if (last && ToSeconds(now - last) < periodS)
                return false;
            last = now;
            return true;
        }
    }

    bool Enabled()
    {
        ReadIni();
        return g_enabled;
    }

    bool Install(std::string& summary)
    {
        if (g_state)
            return true;
        if (!Enabled())
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
        State* state = new State();
        // The read sites: every global the polls touch, behind the bytes
        // that proved it.
        int sitesOk = 0;
        for (const SiteGroup& group : kSites)
        {
            bool ok = true;
            for (const SiteCheck& check : group.checks)
            {
                SiteRow row;
                row.name = std::string(group.name) + "." + check.name;
                row.rva = static_cast<std::uint32_t>(check.rva);
                row.ok = BytesMatch(g_base + check.rva, check.bytes);
                ok = ok && row.ok;
                state->sites.push_back(row);
            }
            state->siteOk[group.site] = ok;
            if (ok)
                ++sitesOk;
            else
                state->mismatches += std::string(group.name) + ";";
        }
        for (const SeriesInfo& info : kSeries)
        {
            const int site = SiteOf(info.series);
            state->seriesStatus[info.series] = site < 0 || state->siteOk[site] ? "ok" : "site_mismatch";
        }
        for (const PortInfo& info : kPorts)
            state->status[info.port] = "not_installed";
        // The tables exist before the first detour can run.
        g_state = state;
        int installed = 0;
        std::string off;
        for (const HookSpec& spec : kHooks)
        {
            const char* status = HookPort(spec);
            state->status[spec.port] = status;
            if (std::strcmp(status, "installed") == 0 || std::strcmp(status, "shared_render_target") == 0)
                ++installed;
            else
            {
                off += std::string(Info(spec.port).id) + ":" + status + ";";
                state->mismatches += std::string(Info(spec.port).id) + ";";
            }
        }
        SYSTEMTIME local{};
        GetLocalTime(&local);
        state->started = LocalTime(local);
        const std::string name = ReportFileName(local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
            local.wSecond);
        state->reportPath = replay::storage::Path(kReportFolder) + L"\\" + std::wstring(name.begin(), name.end());
        state->startTicks = state->lastReport = Ticks();
        summary = "ports=" + std::to_string(installed) + "/" + std::to_string(kPortCount) + ",off=" +
            (off.empty() ? "none" : off) + ",sites=" + std::to_string(sitesOk) + "/" + std::to_string(kSiteCount) +
            ",report=ReplayDebug\\" + name;
        return true;
    }

    void Tick(const TickInput& input)
    {
        State* state = g_state;
        if (!state)
            return;
        REPLAY_PROFILE_ZONE("probe.capture_tick");
        const std::uint64_t now = Ticks();
        const std::size_t mode = static_cast<std::size_t>(input.mode) < kModes ? static_cast<std::size_t>(input.mode) : 0;
        if (state->lastTick)
            state->modeTicks[CurrentMode() < kModes ? CurrentMode() : 0] += now - state->lastTick;
        state->lastTick = now;
        state->scriptThread = GetCurrentThreadId();
        ++state->frames;
        g_mode.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
        for (PortStats& port : state->ports)
            port.EndFrame();

        Poll poll{*state, mode};
        const std::uint64_t pollStart = Ticks();
        poll.Lightning();
        const bool due30 = Due(state->due30, now, 1.0 / 30.0);
        const bool due10 = Due(state->due10, now, 1.0 / 10.0);
        const bool due4 = Due(state->due4, now, 1.0 / 4.0);
        const bool due2 = Due(state->due2, now, 1.0 / 2.0);
        const bool due1 = Due(state->due1, now, 1.0);
        if (due10 || due4 || due1)
            poll.FindPlayer();
        if (due30)
            poll.Weather();
        if (due10)
        {
            poll.Camera();
            poll.Timecycle();
        }
        if (due4)
            poll.WaterAndVehicle();
        if (due2)
        {
            poll.MapChanges();
            poll.Rayfire();
        }
        if (due1)
        {
            poll.Pickups();
            poll.GlassInterface();
            poll.Cloth();
            poll.Lights();
            poll.Tv();
        }
        state->pollTicks += Ticks() - pollStart;

        if (ToSeconds(now - state->lastReport) >= kReportEveryS)
        {
            state->lastReport = now;
            WriteReport(*state, false);
        }
    }

    void Shutdown()
    {
        State* state = g_state;
        if (!state)
            return;
        // The unload disabled every hook and drained every detour before
        // this (module_lifetime.h): no detour runs any more.
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
        delete state;
    }

    // The render target recorder's detours report C29 and C30.
    void NoteRenderTargetRegister(std::uint64_t name, std::uint8_t flag, bool accepted, void* returnAddress)
    {
        if (!g_state)
            return;
        char text[64] = {};
        const std::uint32_t hash = ReadCString(name, text) ? HashLower(text) : 0;
        Record(kRenderTargetRegister, returnAddress, hash, flag, accepted ? 1 : 0, 0, 0.0f);
        Value(kRenderTargetRegister, 0, accepted ? 1 : 0);
        AddName("render_target", name);
    }

    void NoteRenderTargetLink(std::uint32_t model, void* returnAddress)
    {
        State* state = g_state;
        if (!state)
            return;
        Record(kRenderTargetLink, returnAddress, model, 0, 0, 0, 0.0f);
        state->renderTargetLinks.Add(model);
    }
}
