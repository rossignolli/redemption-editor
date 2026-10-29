#include "mix_scene.h"
#include "mix_scene_core.h"
#include "mix_scene_pool_core.h"
#include "speech_duck_core.h"

#include "entity_cache.h"
#include "log_policy.h"
#include "guarded_memory.h"
#include "replay_playback.h"
#include "replay_recording.h"
#include "sound_census.h"
#include "world_theatre_engine.h"

#include <windows.h>

#include "main.h"
#include "natives.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <ostream>
#include <string>
#include <vector>

// The mixer scenes and the audio mix groups, engine side
// (mix_scene.h, mix_scene_core.h). Every internal function is checked at its
// RVA (24 prologue bytes, build 1491.50 dump) and by the native body that
// calls it (the lea of its global and the call site), called under SEH from
// the script thread; a mismatch turns the replay part off for the session
// and logs MIX_SCENE_CHECK.

namespace replay::mixscene
{
    namespace
    {
        Config g_config;

        std::uintptr_t Base() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }

        // ---- signatures and sites ----------------------------------------

        struct Prologue
        {
            const char* name;
            std::uint32_t rva;
            std::uint8_t bytes[24];
        };
        // 0x2FC2200(conversation): the conversation duck's start (only
        // caller 0x2FCC169); reads +0xE8, +0x30, +0x38 and the participants'
        // +0x7C/+0x98, nothing else of the conversation.
        constexpr Prologue kConversationStart{"ConversationSceneStart", 0x2FC2200,
            {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57,
                0x48, 0x8D, 0x6C, 0x24, 0xC9, 0x48}};
        // 0x2FC2964(conversation): stops +0xE8 (0x327A07C(scene, 0)) and zeroes it.
        constexpr Prologue kConversationStop{"ConversationSceneStop", 0x2FC2964,
            {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89, 0xE8, 0x00, 0x00, 0x00, 0x48, 0x85,
                0xC9, 0x74, 0x0F, 0x33, 0xD2, 0xE8}};
        // START_AUDIO_SCENE's 0x31C8910(script audio 0x5C81C20, hash) -> bool
        // and STOP_AUDIO_SCENE's 0x31C946C(script audio, hash): the calling
        // script's scenes (STOP_AUDIO_SCENES clears them at the theatre exit).
        constexpr Prologue kScriptStart{"ScriptSceneStart", 0x31C8910,
            {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0xE8, 0xF0, 0x23,
                0xFF, 0xFF, 0x85, 0xC0, 0x79, 0x04}};
        constexpr Prologue kScriptStop{"ScriptSceneStop", 0x31C946C,
            {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0xE8, 0x90, 0xC5,
                0xD1, 0xFD, 0x48, 0x85, 0xC0, 0x74}};
        // ADD_ENTITY_TO_AUDIO_MIX_GROUP's 0x2EE52E8(mixer 0x5B98760, entity,
        // slot, hash, fade): resolves the entity's audio itself (0x2EF31CC)
        // and refuses a hash with no mix group metadata (0x252B584 type 4).
        constexpr Prologue kGroupAdd{"MixGroupAdd", 0x2EE52E8,
            {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x57, 0x41, 0x54,
                0x41, 0x55, 0x41, 0x56, 0x41, 0x57}};
        // The mixer's own start and stop (the theatre hooks them after the same
        // check; called through the hook), the release of the start's temporary
        // list 0x2F03DAC (every start site calls it right after: 0x31C88F0,
        // 0x31CEFD9) and the scene's variable set 0x3279628(scene, name hash,
        // value) (the engine's 0x2F2990A; SET_AUDIO_SCENE_VARIABLE's by-name
        // 0x3279674 jumps to it).
        constexpr Prologue kMixerStart{"MixerStartScene", 0x2F19018,
            {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x41, 0x56, 0x48,
                0x83, 0xEC, 0x40, 0x49, 0x8B, 0xF0}};
        constexpr Prologue kMixerStop{"MixerStopScene", 0x327A07C,
            {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78,
                0x20, 0x41, 0x56, 0x48, 0x83, 0xEC}};
        constexpr Prologue kTempRelease{"SceneStartTempRelease", 0x2F03DAC,
            {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78,
                0x20, 0x41, 0x56, 0x48, 0x83, 0xEC}};
        constexpr Prologue kVarSet{"SceneSetVariable", 0x3279628,
            {0x4C, 0x8D, 0x41, 0x18, 0x4D, 0x63, 0x50, 0x50, 0x49, 0x8B, 0xC0, 0x4F, 0x8D, 0x0C, 0xD0, 0x4D, 0x3B, 0xC1,
                0x74, 0x0D, 0x39, 0x10, 0x74, 0x2B}};

        // A lea rcx,[rip+disp32] (7 bytes) or a call rel32 (5 bytes) whose
        // target is checked.
        struct Site
        {
            const char* name;
            std::uint32_t rva;
            bool lea; // false: call (or jmp)
            std::uint32_t target;
            bool jmp = false; // a jmp rel32 (5 bytes) in place of the call
        };
        constexpr std::uint32_t kMixerRva = 0x5B98760;
        constexpr std::uint32_t kScriptAudioRva = 0x5C81C20;
        constexpr Site kSites[] = {
            {"conversation_start_mixer", 0x2FC231E, true, kMixerRva},
            {"conversation_start_call", 0x2FC2330, false, 0x2F19018},
            {"conversation_stop_call", 0x2FC297B, false, 0x327A07C},
            {"start_audio_scene_audio", 0xF77B18, true, kScriptAudioRva},
            {"start_audio_scene_call", 0xF77B1F, false, kScriptStart.rva},
            {"stop_audio_scene_audio", 0xF78232, true, kScriptAudioRva},
            {"stop_audio_scene_call", 0xF78239, false, kScriptStop.rva},
            {"mix_group_native_mixer", 0xF65EB5, true, kMixerRva},
            {"mix_group_native_call", 0xF65EE1, false, kGroupAdd.rva},
            // The hash variants START (0xDC2F83A0612CA34D, handler 0xF65254)
            // and STOP (0x6AB944DF68B512D3, handler 0xF653E4).
            {"start_scene_hash_audio", 0xF65261, true, kScriptAudioRva},
            {"start_scene_hash_call", 0xF6526A, false, kScriptStart.rva},
            {"stop_scene_hash_audio", 0xF653E8, true, kScriptAudioRva},
            {"stop_scene_hash_jmp", 0xF653F1, false, kScriptStop.rva, true},
        };
        // The code scenes' direct start (MixSceneReplayCode).
        constexpr Site kCodeSites[] = {
            {"zone_start_mixer", 0x31CEF8B, true, kMixerRva},
            {"zone_start_call", 0x31CEFCF, false, kMixerStart.rva},
            {"zone_temp_release", 0x31CEFD9, false, kTempRelease.rva},
            {"script_temp_release", 0x31C88F0, false, kTempRelease.rva},
        };
        constexpr Site kVarSites[] = {
            {"var_set_call", 0x2F2990A, false, kVarSet.rva},
        };
        // The native's fade: its argument when above 0x330F380, else 0x330F494.
        constexpr std::uint32_t kFadeFloorRva = 0x330F380;
        constexpr std::uint32_t kFadeDefaultRva = 0x330F494;

        bool PrologueAt(const Prologue& p)
        {
            std::uint8_t code[24]{};
            return replay::mem::Copy(code, Base() + p.rva, sizeof(code)) && std::memcmp(code, p.bytes, sizeof(code)) == 0;
        }

        bool SiteAt(const Site& site)
        {
            std::uint8_t code[7]{};
            const std::size_t size = site.lea ? 7 : 5;
            if (!replay::mem::Copy(code, Base() + site.rva, size))
                return false;
            std::int32_t disp = 0;
            if (site.lea)
            {
                if (code[0] != 0x48 || code[1] != 0x8D || code[2] != 0x0D)
                    return false;
                std::memcpy(&disp, code + 3, 4);
            }
            else
            {
                if (code[0] != (site.jmp ? 0xE9 : 0xE8))
                    return false;
                std::memcpy(&disp, code + 1, 4);
            }
            return static_cast<std::int64_t>(site.rva) + static_cast<std::int64_t>(size) + disp == site.target;
        }

        bool g_checked = false;
        bool g_callsOk = false;
        std::string g_checkFailed = "none";
        float g_fade = 0.0f;
        bool g_codeOk = false;
        std::string g_codeFailed = "none";
        bool g_varsOk = false;
        std::string g_varsFailed = "none";

        bool CallsOk(std::ostream& log)
        {
            if (g_checked)
                return g_callsOk;
            g_checked = true;
            for (const Prologue* p : {&kConversationStart, &kConversationStop, &kScriptStart, &kScriptStop, &kGroupAdd})
                if (!PrologueAt(*p) && g_checkFailed == "none")
                    g_checkFailed = p->name;
            for (const Site& site : kSites)
                if (!SiteAt(site) && g_checkFailed == "none")
                    g_checkFailed = site.name;
            float floor = 0.0f;
            float fallback = 0.0f;
            if (g_checkFailed == "none" && (!replay::mem::Read(Base() + kFadeFloorRva, floor) ||
                                               !replay::mem::Read(Base() + kFadeDefaultRva, fallback)))
                g_checkFailed = "fade_constants";
            g_fade = 0.0f > floor ? 0.0f : fallback; // the native with p2 = 0
            g_callsOk = g_checkFailed == "none";
            // The code scenes' direct start and the variables. A
            // hooked function's prologue is the hook's jump: the theatre
            // checked the same 24 bytes before installing it.
            if (!replay::theatre::SceneStartInstalled() && !PrologueAt(kMixerStart))
                g_codeFailed = kMixerStart.name;
            else if (!replay::theatre::SceneStopInstalled() && !PrologueAt(kMixerStop))
                g_codeFailed = kMixerStop.name;
            else if (!PrologueAt(kTempRelease))
                g_codeFailed = kTempRelease.name;
            for (const Site& site : kCodeSites)
                if (g_codeFailed == "none" && !SiteAt(site))
                    g_codeFailed = site.name;
            g_codeOk = g_codeFailed == "none";
            if (!PrologueAt(kVarSet))
                g_varsFailed = kVarSet.name;
            for (const Site& site : kVarSites)
                if (g_varsFailed == "none" && !SiteAt(site))
                    g_varsFailed = site.name;
            g_varsOk = g_varsFailed == "none";
            log << "MIX_SCENE_CHECK,ok=" << (g_callsOk ? 1 : 0) << ",failed=" << g_checkFailed << ",fade=" << g_fade
                << ",start_detour=" << (replay::theatre::SceneStartInstalled() ? 1 : 0)
                << ",stop_detour=" << (replay::theatre::SceneStopInstalled() ? 1 : 0) << ",code_ok=" << (g_codeOk ? 1 : 0)
                << ",code_failed=" << g_codeFailed << ",vars_ok=" << (g_varsOk ? 1 : 0) << ",vars_failed=" << g_varsFailed
                << '\n';
            return g_callsOk;
        }

        // ---- SEH wrappers (no objects with destructors) --------------------

        using ConversationFn = void(__fastcall*)(void*);
        using GroupAddFn = void(__fastcall*)(void*, void*, std::uint32_t, std::uint32_t, float);

        bool CallConversation(std::uintptr_t function, void* conversation, std::uint32_t& code)
        {
            __try
            {
                reinterpret_cast<ConversationFn>(function)(conversation);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The mixer's start (mixer, &hash, &reference, owner 0,
        // start offset 0, 0, &temporary list) as the code sites call it
        // (0x31CEFCF, 0x3211FE2), then the list's release (0x2F03DAC).
        using MixerStartFn = std::uint8_t(__fastcall*)(void*, const std::uint32_t*, std::uint64_t*, void*, std::uint32_t, void*, void*);
        using TempReleaseFn = void(__fastcall*)(void*);
        using MixerStopFn = std::uint64_t(__fastcall*)(std::uint64_t, std::uint64_t);
        using VarSetFn = void(__fastcall*)(std::uint64_t, std::uint32_t, float);

        bool CallMixerStart(std::uint32_t hash, std::uint64_t* reference, std::uint8_t& accepted, std::uint32_t& code)
        {
            std::uint64_t temp[3] = {}; // 0x18 bytes, zeroed as at the code sites
            const std::uint32_t name = hash;
            __try
            {
                accepted = reinterpret_cast<MixerStartFn>(Base() + kMixerStart.rva)(
                    reinterpret_cast<void*>(Base() + kMixerRva), &name, reference, nullptr, 0, nullptr, temp);
                reinterpret_cast<TempReleaseFn>(Base() + kTempRelease.rva)(temp);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool CallMixerStop(std::uint64_t scene, std::uint32_t& code)
        {
            __try
            {
                reinterpret_cast<MixerStopFn>(Base() + kMixerStop.rva)(scene, 0);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool CallVarSet(std::uint64_t scene, std::uint32_t name, float value, std::uint32_t& code)
        {
            __try
            {
                reinterpret_cast<VarSetFn>(Base() + kVarSet.rva)(scene, name, value);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The scene's reference and hash (rung 2; false when unreadable).
        bool ReadScene(std::uint64_t scene, std::uint64_t& reference, std::uint32_t& hash)
        {
            reference = 0;
            hash = 0;
            if (!scene)
                return false;
            return replay::mem::Read(static_cast<std::uintptr_t>(scene + kSceneReference), reference) &&
                replay::mem::Read(static_cast<std::uintptr_t>(scene + kSceneHash), hash);
        }
        bool Alive(std::uint64_t scene, std::uint32_t instanceHash)
        {
            std::uint64_t reference = 0;
            std::uint32_t hash = 0;
            return ReadScene(scene, reference, hash) && SceneAlive(hash, reference, instanceHash);
        }

        bool CallGroupAdd(std::uintptr_t function, void* mixer, void* entity, std::uint32_t hash, float fade, std::uint32_t& code)
        {
            __try
            {
                reinterpret_cast<GroupAddFn>(function)(mixer, entity, 0, hash, fade);
                return true;
            }
            __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // ---- the replay's conversation objects ------------------------------
        // Plain memory the replay owns (never freed: the mixer keeps the
        // address of +0xE8 as the scene's reference and may clear it when the
        // scene ends, so it must outlive the DLL; VirtualAlloc, not the CRT).
        constexpr std::size_t kFakeCount = 8;
        constexpr std::size_t kFakeStride = 0x800;
        constexpr std::size_t kFakeParts = 0x100; // participants after the header
        std::uint8_t* g_fakes = nullptr;
        bool g_fakeUsed[kFakeCount] = {};

        std::uint8_t* FakeAt(std::size_t index) { return g_fakes ? g_fakes + index * kFakeStride : nullptr; }

        bool IsFakeSlot(std::uint64_t slot)
        {
            if (!g_fakes || !slot)
                return false;
            const std::uintptr_t from = reinterpret_cast<std::uintptr_t>(g_fakes);
            return slot >= from && slot < from + kFakeCount * kFakeStride;
        }

        int TakeFake(const Instance& instance)
        {
            if (!g_fakes)
            {
                g_fakes = static_cast<std::uint8_t*>(
                    VirtualAlloc(nullptr, kFakeCount * kFakeStride, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                if (!g_fakes)
                    return -1;
            }
            for (std::size_t n = 0; n < kFakeCount; ++n)
            {
                if (g_fakeUsed[n])
                    continue;
                std::uint8_t* fake = FakeAt(n);
                std::memset(fake, 0, kFakeStride);
                std::uint8_t* parts = fake + kFakeParts;
                const std::uint64_t partsAddress = reinterpret_cast<std::uint64_t>(parts);
                const std::uint16_t count = instance.tagCount;
                std::memcpy(fake + replay::theatre::kConversationParticipants, &partsAddress, sizeof(partsAddress));
                std::memcpy(fake + replay::theatre::kConversationParticipantCount, &count, sizeof(count));
                for (std::uint8_t t = 0; t < instance.tagCount; ++t)
                {
                    std::uint8_t* part = parts + t * replay::theatre::kParticipantStride;
                    std::memcpy(part + replay::theatre::kParticipantTag, &instance.tags[t], sizeof(std::uint32_t));
                    part[replay::theatre::kParticipantFlag] = 1;
                }
                g_fakeUsed[n] = true;
                return static_cast<int>(n);
            }
            return -1;
        }

        // The references of the code scenes the replay starts (the mixer
        // writes the scene there and keeps its address at scene +0x310,
        // clearing it on stop): replay memory, never freed.
        constexpr std::size_t kRefCount = 64;
        std::uint64_t* g_refs = nullptr;
        bool g_refUsed[kRefCount] = {};

        bool IsRefSlot(std::uint64_t slot)
        {
            if (!g_refs || !slot)
                return false;
            const std::uintptr_t from = reinterpret_cast<std::uintptr_t>(g_refs);
            return slot >= from && slot < from + kRefCount * sizeof(std::uint64_t);
        }

        int TakeRef()
        {
            if (!g_refs)
            {
                g_refs = static_cast<std::uint64_t*>(
                    VirtualAlloc(nullptr, kRefCount * sizeof(std::uint64_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                if (!g_refs)
                    return -1;
            }
            for (std::size_t n = 0; n < kRefCount; ++n)
            {
                // A slot the mixer may still reference (a stop that failed) stays taken.
                if (g_refUsed[n] || g_refs[n])
                    continue;
                g_refUsed[n] = true;
                return static_cast<int>(n);
            }
            return -1;
        }

        std::uint64_t FakeScene(int index)
        {
            std::uint64_t scene = 0;
            if (index >= 0 && g_fakes)
                std::memcpy(&scene, FakeAt(static_cast<std::size_t>(index)) + replay::theatre::kConversationSceneSlot, sizeof(scene));
            return scene;
        }

        // ---- the scene pool -------------------------------------------------
        // mix_scene_pool_core.h: every scene the engine has, read directly
        // (rung 2, guarded copies) after the pool's code sites, the ready to
        // delete rule and the drivers' sites matched once.

        bool g_poolChecked = false;
        bool g_poolOk = false;
        std::string g_poolFailed = "none";
        bool g_driverOk[pool::kDriverCount] = {};

        bool PoolBytesAt(std::uint32_t rva, const std::uint8_t* bytes, std::size_t size)
        {
            std::uint8_t code[96]{};
            return size <= sizeof(code) && replay::mem::Copy(code, Base() + rva, size) && std::memcmp(code, bytes, size) == 0;
        }

        bool PoolOk(std::ostream& log)
        {
            if (g_poolChecked)
                return g_poolOk;
            g_poolChecked = true;
            for (const pool::CodeSite& site : pool::kSites)
                if (g_poolFailed == "none" &&
                    (!PoolBytesAt(site.rva, site.bytes, site.size) || (site.ripAt >= 0 && pool::SiteTarget(site) != site.target)))
                    g_poolFailed = site.name;
            if (g_poolFailed == "none" &&
                !PoolBytesAt(pool::kReadyToDeleteRva, pool::kReadyToDeleteSignature, sizeof(pool::kReadyToDeleteSignature)))
                g_poolFailed = "ready_to_delete";
            g_poolOk = g_poolFailed == "none";
            std::string drivers;
            for (std::size_t n = 0; n < pool::kDriverCount; ++n)
            {
                const pool::Driver& d = pool::kDrivers[n];
                g_driverOk[n] = g_poolOk && pool::DriverConsistent(d) && PoolBytesAt(d.callRva, d.callBytes, 5) &&
                    PoolBytesAt(d.leaRva, d.leaBytes, 7) && PoolBytesAt(d.hashRva, d.hashBytes, d.hashSize);
                if (g_driverOk[n] && d.via == pool::Via::Wrapper)
                {
                    bool tolerant = false;
                    for (const pool::Tolerance& t : pool::kTolerances)
                        if (t.hash == d.hash && pool::ToleranceTarget(t) == d.refRva && PoolBytesAt(t.rva, t.bytes, t.size))
                            tolerant = true;
                    g_driverOk[n] = tolerant;
                }
                if (!g_driverOk[n])
                    drivers += (drivers.empty() ? "" : "|") + std::string(d.name);
            }
            log << "MIX_SCENE_POOL,phase=check,ok=" << (g_poolOk ? 1 : 0) << ",failed=" << g_poolFailed
                << ",sites=" << pool::kSiteCount << ",drivers=" << pool::kDriverCount
                << ",drivers_failed=" << (drivers.empty() ? "none" : drivers) << '\n';
            return g_poolOk;
        }

        bool DriverOk(const pool::Driver& d)
        {
            const std::size_t n = static_cast<std::size_t>(&d - pool::kDrivers);
            return n < pool::kDriverCount && g_driverOk[n];
        }

        struct PoolCache
        {
            bool ok = false;
            std::uint64_t wall = 0;
            std::uint32_t slots = 0;
            std::uint32_t used = 0;
            std::uint32_t dying = 0;
            std::vector<pool::PoolScene> scenes; // alive (in use, not ready to delete)
        };
        PoolCache g_pool;
        std::uint64_t g_poolScans = 0, g_poolFailures = 0;

        std::uint32_t ImageRva(std::uint64_t address)
        {
            const std::uintptr_t base = Base();
            return address >= base && address - base < 0x10000000ull ? static_cast<std::uint32_t>(address - base) : 0u;
        }

        bool ScanPool(PoolCache& out)
        {
            out.scenes.clear();
            out.slots = out.used = out.dying = 0;
            std::uint8_t valid = 0;
            std::uint64_t value = 0, key = 0;
            if (!replay::mem::Read(Base() + pool::kCellValidRva, valid) || !valid ||
                !replay::mem::Read(Base() + pool::kCellValueRva, value) || !replay::mem::Read(Base() + pool::kCellKeyRva, key))
                return false;
            const std::uint64_t object = pool::Decode(value, key);
            std::uint64_t base = 0, flags = 0;
            std::int32_t count = 0, stride = 0;
            if (!replay::mem::Read(static_cast<std::uintptr_t>(object + pool::kPoolBase), base) ||
                !replay::mem::Read(static_cast<std::uintptr_t>(object + pool::kPoolFlags), flags) ||
                !replay::mem::Read(static_cast<std::uintptr_t>(object + pool::kPoolCount), count) ||
                !replay::mem::Read(static_cast<std::uintptr_t>(object + pool::kPoolStride), stride) || !base || !flags ||
                count <= 0 || static_cast<std::uint32_t>(count) > pool::kMaxSlots || stride < static_cast<std::int32_t>(pool::kSceneSize) ||
                stride > 0x1000)
                return false;
            static std::uint8_t slotFlags[pool::kMaxSlots];
            if (!replay::mem::Copy(slotFlags, static_cast<std::uintptr_t>(flags), static_cast<std::size_t>(count)))
                return false;
            out.slots = static_cast<std::uint32_t>(count);
            static std::uint8_t copy[pool::kSceneSize];
            for (std::int32_t i = 0; i < count; ++i)
            {
                if (slotFlags[i] & pool::kSlotFree)
                    continue;
                ++out.used;
                const std::uint64_t scene = base + static_cast<std::uint64_t>(i) * static_cast<std::uint64_t>(stride);
                if (!replay::mem::Copy(copy, static_cast<std::uintptr_t>(scene), sizeof(copy)))
                    continue;
                if (pool::ReadyToDelete(copy))
                {
                    ++out.dying;
                    continue;
                }
                pool::PoolScene s;
                s.slot = static_cast<std::uint32_t>(i);
                s.scene = scene;
                s.settings = pool::Qword(copy, pool::kSceneSettings);
                s.reference = pool::Qword(copy, pool::kSceneReference);
                s.owner = pool::Qword(copy, pool::kSceneOwner);
                pool::NameScene(s, pool::Dword(copy, pool::kSceneHash), ImageRva(s.reference));
                if (s.driver && !DriverOk(*s.driver) && s.name == pool::Name::Direct)
                {
                    s.hash = 0; // a driver whose sites did not match does not name a scene
                    s.name = pool::Name::None;
                    s.driver = nullptr;
                }
                s.patches = pool::Patches(copy);
                s.hasApply = pool::VarOf(copy, pool::kApplyVar, s.apply);
                s.replayOwned = IsRefSlot(s.reference) || IsFakeSlot(s.reference);
                out.scenes.push_back(s);
            }
            return true;
        }

        // The pool, scanned again when older than maxAgeMs (kPoolKeep: the
        // scan this tick already made; the callers scan right after they
        // applied the start and stop events, so the live set and the scan
        // agree: a scene whose start was processed is in it).
        constexpr std::uint64_t kPoolKeep = ~0ull;
        const PoolCache& Pool(std::ostream& log, std::uint64_t maxAgeMs)
        {
            const std::uint64_t wall = GetTickCount64();
            if (g_pool.wall && wall - g_pool.wall < maxAgeMs)
                return g_pool;
            g_pool.wall = wall;
            if (!PoolOk(log))
            {
                g_pool.ok = false;
                return g_pool;
            }
            g_pool.ok = ScanPool(g_pool);
            ++g_poolScans;
            if (!g_pool.ok && g_poolFailures++ < 8)
                log << "MIX_SCENE_POOL,phase=scan,ok=0,failures=" << g_poolFailures << '\n';
            return g_pool;
        }

        // The alive scene at this pointer in the last scan (null: not alive).
        const pool::PoolScene* InPool(std::uint64_t scene)
        {
            for (const pool::PoolScene& s : g_pool.scenes)
                if (s.scene == scene)
                    return &s;
            return nullptr;
        }

        // A scene the replay is running now (by pointer; the script scenes the
        // replay starts keep the script slot's reference).
        bool RunningScene(std::uint64_t scene);

        std::string PoolText(const PoolCache& p, std::size_t& alive)
        {
            std::string text;
            char item[96]{};
            alive = 0;
            for (const pool::PoolScene& s : p.scenes)
            {
                ++alive;
                std::snprintf(item, sizeof(item), "%s0x%x:%s:%s:%u:%s:%s", text.empty() ? "" : "|", s.hash, pool::NameText(s.name),
                    s.hasApply ? std::to_string(s.apply).substr(0, 5).c_str() : "na", s.patches,
                    s.driver ? s.driver->name : "-", s.replayOwned || RunningScene(s.scene) ? "replay" : "engine");
                if (text.size() < 3000)
                    text += item;
            }
            return text.empty() ? std::string("none") : text;
        }

        std::uint64_t g_nextPoolLogWall = 0;
        void PoolSnapshot(Context& context, const char* phase, std::uint32_t clipMs, bool force)
        {
            if (!g_config.poolLog)
                return;
            const std::uint64_t wall = GetTickCount64();
            if (!force && wall < g_nextPoolLogWall)
                return;
            g_nextPoolLogWall = wall + 1000;
            const PoolCache& p = Pool(context.log, 0);
            if (!p.ok)
                return;
            std::size_t alive = 0;
            const std::string text = PoolText(p, alive);
            std::size_t dups = 0;
            for (const pool::PoolScene& s : p.scenes)
                if (s.hash && pool::Duplicates(p.scenes, s.hash) > 1)
                    ++dups;
            context.log << "MIX_SCENE_POOL,phase=" << phase << ",event=snapshot,clip_ms=" << clipMs << ",slots=" << p.slots
                        << ",used=" << p.used << ",dying=" << p.dying << ",alive=" << alive << ",dup=" << dups
                        << ",scenes=" << text << '\n';
        }

        // ---- the live scenes and the events ----------------------------------

        Live g_live;
        std::vector<std::uint32_t> g_playLogged; // hashes whose start this playback logged
        std::uint64_t g_staleStops = 0, g_vanished = 0, g_varSamples = 0;
        std::uint64_t g_eventCursor = 0;
        std::vector<replay::theatre::MixSceneEvent> g_pending;
        std::uint64_t g_lastRecordWall = 0;
        replay::log::LogLimit g_eventLogs;
        std::uint64_t g_starts = 0, g_stops = 0, g_lostStops = 0, g_mineSeen = 0;
            std::uint64_t g_claimed = 0, g_discovered = 0, g_adopted = 0; // the pool
        std::uint64_t g_kindStarts[kKindCount] = {};

        std::uint64_t Wall() { return GetTickCount64(); }

        void Drain()
        {
            static std::vector<replay::theatre::MixSceneEvent> fresh;
            replay::theatre::CollectMixSceneEvents(g_eventCursor, fresh);
            g_pending.insert(g_pending.end(), fresh.begin(), fresh.end());
            if (g_pending.size() > 4096) // nobody consumed them: keep the newest
                g_pending.erase(g_pending.begin(), g_pending.end() - 2048);
        }

        // The events the replay's own start or stop just made (same thread,
        // synchronous) are taken out before Process sees them.
        std::uint64_t g_ownDropped = 0;
        void DropOwnEvents(bool stop, std::uint64_t scene, std::uint32_t hash)
        {
            Drain();
            for (std::size_t n = g_pending.size(); n-- > 0;)
            {
                const replay::theatre::MixSceneEvent& event = g_pending[n];
                const bool mine = stop ? (event.stop && scene && event.scene == scene)
                                       : (!event.stop && event.hash == hash &&
                                             (event.caller == kCallerScript || IsFakeSlot(event.slot)));
                if (mine)
                {
                    g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(n));
                    ++g_ownDropped;
                    return;
                }
            }
        }

        std::string TagText(const std::uint32_t* tags, std::size_t count)
        {
            std::string text;
            char item[16]{};
            for (std::size_t n = 0; n < count; ++n)
            {
                std::snprintf(item, sizeof(item), "%s0x%x", n ? ";" : "", tags[n]);
                text += item;
            }
            return text.empty() ? std::string("none") : text;
        }

        // Applies the pending events to the live set (and the clip's track).
        void Process(Context& context, Track* track, std::uint32_t nowMs, const char* phase)
        {
            for (const replay::theatre::MixSceneEvent& event : g_pending)
            {
                if (!event.stop)
                {
                    if (!event.scene)
                        continue; // the start made no scene
                    if (IsFakeSlot(event.slot) || IsRefSlot(event.slot))
                    {
                        ++g_mineSeen;
                        continue; // the replay's own conversation or code scene
                    }
                    // A scene adopted from the pool before its start event was
                    // drained keeps its instance.
                    if (const std::uint32_t claimed = g_live.Claim(event.scene, event.hash, event.caller))
                    {
                        if (track)
                            if (Instance* adopted = track->Find(claimed))
                            {
                                adopted->caller = event.caller;
                                adopted->kind = KindOfCaller(event.caller);
                            }
                        ++g_claimed;
                        continue;
                    }
                    const Instance& instance = g_live.Start(event.scene, event.hash, event.caller, nowMs, event.tags, event.tagCount);
                    ++g_starts;
                    ++g_kindStarts[static_cast<std::uint8_t>(instance.kind)];
                    if (track)
                        TrackStart(*track, instance);
                    // In a playback, the first start of each hash is always
                    // logged (which code scenes the engine starts by itself).
                    bool firstInPlay = false;
                    if (!track && std::strcmp(phase, "play") == 0 &&
                        std::find(g_playLogged.begin(), g_playLogged.end(), event.hash) == g_playLogged.end())
                    {
                        g_playLogged.push_back(event.hash);
                        firstInPlay = true;
                    }
                    if (g_eventLogs.Take(400) || instance.kind != Kind::Code || firstInPlay)
                        context.log << "MIX_SCENE,phase=" << phase << ",event=start,id=" << instance.id << ",hash=0x" << std::hex
                                    << event.hash << ",caller=0x" << event.caller << std::dec << ",kind=" << KindName(instance.kind)
                                    << ",participants=" << event.participants << ",tags=" << TagText(event.tags, event.tagCount)
                                    << ",clip_ms=" << nowMs << ",recorded=" << (track ? 1 : 0) << '\n';
                }
                else
                {
                    // A stop of a scene that no longer carries the
                    // instance's hash is a stale reference (logged; the scene
                    // had gone already, the poll normally ends it first).
                    const auto open = g_live.open.find(event.scene);
                    const bool stale = open != g_live.open.end() && StaleStop(open->second, event.hash);
                    const std::uint32_t id = g_live.Stop(event.scene);
                    ++g_stops;
                    if (!id)
                        ++g_lostStops;
                    if (stale)
                        ++g_staleStops;
                    if (track)
                        TrackStop(*track, id, nowMs);
                    if (id && (g_eventLogs.Take(400) || stale))
                        context.log << "MIX_SCENE,phase=" << phase << ",event=stop,id=" << id << ",hash=0x" << std::hex
                                    << event.hash << ",caller=0x" << event.caller << std::dec << ",stale=" << (stale ? 1 : 0)
                                    << ",clip_ms=" << nowMs << ",recorded=" << (track ? 1 : 0) << '\n';
                }
            }
            g_pending.clear();
        }

        // ---- the open scenes polled (vanished, variables) ----------------------

        struct VarLast
        {
            float value = 0.0f;
            std::uint32_t storedMs = 0;
        };
        std::map<std::uint64_t, VarLast> g_varLast; // (instance id << 32 | name) -> last stored
        replay::log::LogLimit g_varLogs;
        constexpr std::uint32_t kVarMinIntervalMs = 50;

        void PollLive(Context& context, Track* track, std::uint32_t nowMs, const char* phase, bool vars,
            std::uint64_t poolMaxAgeMs = kPoolKeep)
        {
            // With the pool readable, a scene is alive while its slot is in
            // use, not ready to delete and carries the instance's hash
            // (+0x328, or its driver's reference for a direct scene), whatever
            // its reference (a script scene started without one is not gone).
            const bool poolKnown = g_config.poolRecord && Pool(context.log, poolMaxAgeMs).ok;
            for (auto it = g_live.open.begin(); it != g_live.open.end();)
            {
                const std::uint64_t scene = it->first;
                const Instance& instance = it->second;
                std::uint64_t reference = 0;
                std::uint32_t hash = 0;
                const bool read = ReadScene(scene, reference, hash);
                bool alive = read && SceneAlive(hash, reference, instance.hash);
                if (poolKnown)
                {
                    const pool::PoolScene* inPool = InPool(scene);
                    alive = inPool && inPool->hash == instance.hash;
                }
                if (!alive)
                {
                    ++g_vanished;
                    if (track)
                        TrackStop(*track, instance.id, nowMs);
                    context.log << "MIX_SCENE,phase=" << phase << ",event=vanished,id=" << instance.id << ",hash=0x" << std::hex
                                << instance.hash << ",hash_now=0x" << hash << ",reference=0x" << reference << std::dec
                                << ",kind=" << KindName(instance.kind) << ",read=" << (read ? 1 : 0) << ",pool=" << (poolKnown ? 1 : 0)
                                << ",clip_ms=" << nowMs << ",recorded=" << (track ? 1 : 0) << '\n';
                    it = g_live.open.erase(it);
                    continue;
                }
                if (track && vars)
                {
                    std::int32_t count = 0;
                    std::uint32_t pairs[kMaxVariables * 2] = {};
                    if (replay::mem::Read(static_cast<std::uintptr_t>(scene + kSceneVariableCount), count) && count > 0)
                    {
                        const std::size_t n = static_cast<std::size_t>(count) < kMaxVariables ? static_cast<std::size_t>(count)
                                                                                              : kMaxVariables;
                        if (replay::mem::Copy(pairs, static_cast<std::uintptr_t>(scene + kSceneVariables), n * 8))
                            for (std::size_t v = 0; v < n; ++v)
                            {
                                const std::uint32_t name = pairs[v * 2];
                                float value = 0.0f;
                                std::memcpy(&value, &pairs[v * 2 + 1], sizeof(value));
                                if (!name || !std::isfinite(value))
                                    continue;
                                const std::uint64_t key = (static_cast<std::uint64_t>(instance.id) << 32) | name;
                                const auto found = g_varLast.find(key);
                                const bool seen = found != g_varLast.end();
                                if (!VarChanged(seen, seen ? found->second.value : 0.0f, value))
                                    continue;
                                if (seen && nowMs - found->second.storedMs < kVarMinIntervalMs && nowMs >= found->second.storedMs)
                                    continue;
                                g_varLast[key] = VarLast{value, nowMs};
                                track->vars.push_back(VarSample{nowMs, instance.id, name, value});
                                ++g_varSamples;
                                if (g_varLogs.Take(200) || !seen)
                                    context.log << "MIX_SCENE_VAR,phase=record,id=" << instance.id << ",hash=0x" << std::hex
                                                << instance.hash << ",name=0x" << name << std::dec << ",value=" << value
                                                << ",clip_ms=" << nowMs << '\n';
                            }
                    }
                }
                ++it;
            }
        }

        // ---- the pool's scenes the start detour never saw ----------------------
        // The drivers' direct scenes (named by their reference, caller = the
        // driver's start return) every scan; with `adoptAll` also the scenes
        // started through the wrapper before the hooks or before this
        // session's first scan (caller 0, claimed by their start event if it
        // is still pending).
        bool g_poolSynced = false;
        replay::log::LogLimit g_discoverLogs;

        void Discover(Context& context, Track* track, std::uint32_t nowMs, const char* phase, bool adoptAll,
            std::uint64_t maxAgeMs = kPoolKeep)
        {
            if (!g_config.poolRecord)
                return;
            const PoolCache& p = Pool(context.log, maxAgeMs);
            if (!p.ok)
                return;
            for (const pool::PoolScene& s : p.scenes)
            {
                if (s.replayOwned || !s.hash || g_live.open.count(s.scene))
                    continue;
                std::uint32_t caller = 0;
                if (s.name == pool::Name::Direct && s.driver)
                    caller = s.driver->ReturnRva();
                else if (!adoptAll)
                    continue; // a wrapper start not drained yet: its event comes
                const Instance& instance = g_live.Start(s.scene, s.hash, caller, nowMs, nullptr, 0);
                if (caller)
                    ++g_discovered;
                else
                    ++g_adopted;
                if (track)
                    TrackStart(*track, instance);
                if (g_discoverLogs.Take(400) || s.name == pool::Name::Direct)
                    context.log << "MIX_SCENE,phase=" << phase << ",event=start,via=" << (caller ? "direct" : "adopt")
                                << ",id=" << instance.id << ",hash=0x" << std::hex << s.hash << ",caller=0x" << caller << std::dec
                                << ",driver=" << (s.driver ? s.driver->name : "none") << ",slot=" << s.slot << ",patches=" << s.patches
                                << ",apply=" << (s.hasApply ? std::to_string(s.apply) : std::string("na")) << ",clip_ms=" << nowMs
                                << ",recorded=" << (track ? 1 : 0) << '\n';
            }
            g_poolSynced = true;
        }

        // ---- recording: the mix groups ---------------------------------------

        std::map<std::uint32_t, std::uint32_t> g_groupStored; // entity -> group last put in the track
        std::uint64_t g_nextGroupPollWall = 0;
        constexpr std::uint64_t kGroupPollMs = 250;
        std::uint64_t g_groupChanges = 0;

        void PollGroups(Context& context, Track& track, std::uint32_t nowMs)
        {
            const std::uint64_t wall = Wall();
            if (wall < g_nextGroupPollWall)
                return;
            g_nextGroupPollWall = wall + kGroupPollMs;
            if (track.groups.empty())
                g_groupStored.clear(); // a fresh recording (or every group was none)
            for (const replay::recorder::RecordedEntity& entity : context.recorder.entities)
            {
                if (entity.vehicle || !entity.handle || !replay::entities::Exists(entity.handle) ||
                    !replay::entities::IsPed(entity.handle))
                    continue;
                const std::uint32_t group = static_cast<std::uint32_t>(AUDIO::_GET_ENTITY_AUDIO_MIX_GROUP(entity.handle));
                const std::uint32_t id = entity.id.Packed();
                const auto found = g_groupStored.find(id);
                const std::uint32_t stored = found == g_groupStored.end() ? 0 : found->second;
                if (group == stored)
                    continue;
                g_groupStored[id] = group;
                track.groups.push_back(GroupSample{nowMs, id, group});
                ++g_groupChanges;
                context.log << "MIX_GROUP,phase=record,ped=" << replay::entity::IdText(entity.id) << ",group=0x" << std::hex
                            << group << ",was=0x" << stored << std::dec << ",clip_ms=" << nowMs << '\n';
            }
        }

        // ---- playback -----------------------------------------------------------

        struct Running
        {
            std::uint32_t id = 0;
            Kind kind = Kind::Code;
            std::uint32_t hash = 0;
            int fake = -1;
            int ref = -1;            // code scene: the replay's reference slot
            std::uint64_t scene = 0; // the engine's scene object (the stop event's key)
            Instance instance;
            std::vector<std::pair<std::uint32_t, float>> applied; // variables set on it
        };
        std::vector<Running> g_running;
        bool RunningScene(std::uint64_t scene)
        {
            if (!scene)
                return false;
            for (const Running& r : g_running)
                if (r.scene == scene)
                    return true;
            return false;
        }
        std::vector<std::uint64_t> RunningScenes()
        {
            std::vector<std::uint64_t> out;
            for (const Running& r : g_running)
                if (r.scene)
                    out.push_back(r.scene);
            return out;
        }
        std::map<std::uint32_t, std::uint32_t> g_attempts; // instance id -> starts this playback
        constexpr std::uint32_t kMaxAttempts = 3;
        std::uint64_t g_varSets = 0, g_varFailed = 0, g_engineStopped = 0, g_codeStarts = 0;
        replay::log::LogLimit g_varPlayLogs;

        // The recorded variables in force at timeMs set on the running scene
        // (only the ones that differ from what the replay already set).
        void ApplyVars(Context& context, Running& running, std::uint32_t timeMs)
        {
            if (!g_config.vars || !g_varsOk || !running.scene || running.kind == Kind::Conversation)
                return;
            const auto wanted = context.clip.mixScenes.VarsAt(running.id, timeMs);
            for (const auto& entry : wanted)
            {
                bool same = false;
                for (const auto& done : running.applied)
                    if (done.first == entry.first && !VarChanged(true, done.second, entry.second))
                        same = true;
                if (same)
                    continue;
                if (!Alive(running.scene, running.hash))
                    return;
                std::uint32_t code = 0;
                const bool ok = CallVarSet(running.scene, entry.first, entry.second, code);
                if (ok)
                {
                    ++g_varSets;
                    bool replaced = false;
                    for (auto& done : running.applied)
                        if (done.first == entry.first)
                        {
                            done.second = entry.second;
                            replaced = true;
                        }
                    if (!replaced)
                        running.applied.push_back(entry);
                }
                else
                    ++g_varFailed;
                if (g_varPlayLogs.Take(200) || !ok)
                    context.log << "MIX_SCENE_VAR,phase=play,id=" << running.id << ",hash=0x" << std::hex << running.hash
                                << ",name=0x" << entry.first << std::dec << ",value=" << entry.second << ",ok=" << (ok ? 1 : 0)
                                << ",code=0x" << std::hex << code << std::dec << ",scene=0x" << std::hex << running.scene << std::dec
                                << ",playhead_ms=" << timeMs << '\n';
            }
        }

        // An engine scene open now with the same start site (a code scene the
        // engine chose instead of the recorded one), for the log.
        std::uint32_t SiblingOf(const Instance& instance)
        {
            for (const auto& entry : g_live.open)
                if (entry.second.caller == instance.caller && entry.second.hash != instance.hash)
                    return entry.second.hash;
            return 0;
        }
        std::map<Entity, std::uint32_t> g_applied; // handle -> group the replay put it in
        std::uint64_t g_nextGroupApplyWall = 0;
        std::uint64_t g_playStarts = 0, g_playStops = 0, g_playFailed = 0, g_groupAdds = 0, g_groupRemoves = 0,
            g_groupFailed = 0;
        bool g_playing = false;
        constexpr std::size_t kMaxRunning = 16;

        bool StartScene(Context& context, const Instance& instance, std::uint32_t timeMs)
        {
            Running running;
            running.id = instance.id;
            running.kind = instance.kind;
            running.hash = instance.hash;
            running.instance = instance;
            std::uint32_t code = 0;
            bool ok = false;
            const char* how = "none";
            const char* via = "conversation";
            int thread = 0;
            ++g_attempts[instance.id];
            if (instance.kind == Kind::Conversation)
            {
                running.fake = TakeFake(instance);
                if (running.fake >= 0)
                {
                    ok = CallConversation(Base() + kConversationStart.rva, FakeAt(static_cast<std::size_t>(running.fake)), code) &&
                        FakeScene(running.fake) != 0;
                    how = code ? "exception" : (ok ? "ok" : "no_scene");
                    running.scene = FakeScene(running.fake);
                    if (!ok)
                        g_fakeUsed[running.fake] = false;
                }
                else
                    how = "no_object";
            }
            else if (instance.kind == Kind::Script)
            {
                // Rung 1: the hash variant of START_AUDIO_SCENE
                // on the script thread. It lands in the slot of the thread
                // the ScriptHook runs us in (thread= in the log); true with
                // no start event means that slot already had a scene of the
                // hash (0x31C8820 returns early): "existing".
                thread = SCRIPTS::GET_ID_OF_THIS_THREAD();
                ok = AUDIO::_0xDC2F83A0612CA34D(static_cast<Any>(instance.hash)) != 0;
                how = ok ? "ok" : "refused";
                via = "native";
            }
            else if (instance.kind == Kind::Code)
            {
                // Rung 4: the mixer's own start with a reference of the
                // replay's.
                via = "mixer";
                if (!g_codeOk)
                    how = "unchecked";
                else
                {
                    running.ref = TakeRef();
                    if (running.ref < 0)
                        how = "no_reference";
                    else
                    {
                        std::uint8_t accepted = 0;
                        std::uint64_t* reference = g_refs + running.ref;
                        ok = CallMixerStart(instance.hash, reference, accepted, code) && accepted && *reference != 0;
                        running.scene = *reference;
                        how = code ? "exception" : (ok ? "ok" : (accepted ? "no_scene" : "refused"));
                        if (!ok && !*reference)
                            g_refUsed[running.ref] = false;
                        if (ok)
                            ++g_codeStarts;
                    }
                }
            }
            // The start event the call made is the replay's own.
            bool sawEvent = false;
            {
                Drain();
                for (std::size_t n = g_pending.size(); n-- > 0;)
                    if (!g_pending[n].stop && g_pending[n].hash == instance.hash &&
                        (g_pending[n].caller == kCallerScript || IsFakeSlot(g_pending[n].slot) || IsRefSlot(g_pending[n].slot)))
                    {
                        if (!running.scene)
                            running.scene = g_pending[n].scene;
                        g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(n));
                        ++g_ownDropped;
                        sawEvent = true;
                        break;
                    }
            }
            if (instance.kind == Kind::Script && ok && !sawEvent)
                how = "existing"; // accepted without a new scene
            if (running.fake >= 0 && !running.scene)
                running.scene = FakeScene(running.fake);
            const std::uint32_t sibling = instance.kind == Kind::Code ? SiblingOf(instance) : 0;
            context.log << "MIX_SCENE_PLAY,action=start,id=" << instance.id << ",hash=0x" << std::hex << instance.hash << std::dec
                        << ",kind=" << KindName(instance.kind) << ",via=" << via << ",tags=" << TagText(instance.tags, instance.tagCount)
                        << ",result=" << how << ",code=0x" << std::hex << code << std::dec << ",scene=0x" << std::hex
                        << running.scene << ",caller=0x" << instance.caller << ",sibling=0x" << sibling << std::dec
                        << ",thread=" << thread << ",attempt=" << g_attempts[instance.id] << ",recorded_start_ms=" << instance.startMs
                        << ",playhead_ms=" << timeMs << '\n';
            if (!ok)
            {
                ++g_playFailed;
                return false;
            }
            ++g_playStarts;
            g_running.push_back(running);
            ApplyVars(context, g_running.back(), timeMs);
            return true;
        }

        void StopScene(Context& context, const Running& running, std::uint32_t timeMs, const char* why)
        {
            std::uint32_t code = 0;
            bool ok = false;
            if (running.kind == Kind::Conversation && running.fake >= 0)
            {
                ok = CallConversation(Base() + kConversationStop.rva, FakeAt(static_cast<std::size_t>(running.fake)), code);
                g_fakeUsed[running.fake] = false;
            }
            bool fallback = false;
            if (running.kind == Kind::Script)
            {
                // Rung 1: STOP_AUDIO_SCENE by hash; if the scene
                // the replay started is still alive (its slot was not the one
                // the native looked in), the mixer's stop on it.
                AUDIO::_0x6AB944DF68B512D3(static_cast<Any>(running.hash));
                ok = true;
                if (running.scene && Alive(running.scene, running.hash) &&
                    (replay::theatre::SceneStopInstalled() || PrologueAt(kMixerStop)))
                {
                    fallback = true;
                    ok = CallMixerStop(running.scene, code);
                }
            }
            else if (running.kind == Kind::Code)
            {
                if (running.scene && Alive(running.scene, running.hash))
                    ok = CallMixerStop(running.scene, code);
                else
                    ok = true; // the engine stopped it already
                if (running.ref >= 0 && g_refs && !g_refs[running.ref])
                    g_refUsed[running.ref] = false; // the mixer let go of the reference
            }
            DropOwnEvents(true, running.scene, running.hash);
            ++g_playStops;
            context.log << "MIX_SCENE_PLAY,action=stop,id=" << running.id << ",hash=0x" << std::hex << running.hash << std::dec
                        << ",kind=" << KindName(running.kind) << ",why=" << why << ",result=" << (ok ? "ok" : (code ? "exception" : "skip"))
                        << ",fallback=" << (fallback ? 1 : 0) << ",scene_alive=" << (running.scene && Alive(running.scene, running.hash) ? 1 : 0)
                        << ",playhead_ms=" << timeMs << '\n';
        }

        void StopAll(Context& context, std::uint32_t timeMs, const char* why)
        {
            for (const Running& running : g_running)
                StopScene(context, running, timeMs, why);
            g_running.clear();
        }

        void ApplyGroup(Context& context, Entity handle, std::uint32_t entityId, std::uint32_t recorded, std::uint32_t timeMs,
            const char* who)
        {
            if (!handle || !replay::entities::Exists(handle) || !replay::entities::IsPed(handle))
                return;
            const std::uint32_t current = static_cast<std::uint32_t>(AUDIO::_GET_ENTITY_AUDIO_MIX_GROUP(handle));
            const GroupAction action = DecideGroup(recorded, current, g_applied.count(handle) != 0);
            if (action == GroupAction::None)
                return;
            std::uint32_t code = 0;
            bool ok = false;
            if (action == GroupAction::Add)
            {
                ok = CallGroupAdd(Base() + kGroupAdd.rva, reinterpret_cast<void*>(Base() + kMixerRva),
                    replay::entities::AddressOf(handle), recorded, g_fade, code);
                const std::uint32_t after = static_cast<std::uint32_t>(AUDIO::_GET_ENTITY_AUDIO_MIX_GROUP(handle));
                ok = ok && after == recorded;
                if (ok)
                {
                    g_applied[handle] = recorded;
                    ++g_groupAdds;
                }
                else
                    ++g_groupFailed;
            }
            else
            {
                AUDIO::REMOVE_ENTITY_FROM_AUDIO_MIX_GROUP(handle, 0.0f);
                g_applied.erase(handle);
                ok = true;
                ++g_groupRemoves;
            }
            context.log << "MIX_GROUP,phase=play,who=" << who << ",ped=" << replay::entity::IdText(replay::entity::ReplayId::FromPacked(entityId))
                        << ",action=" << (action == GroupAction::Add ? "add" : "remove") << ",group=0x" << std::hex << recorded
                        << ",was=0x" << current << std::dec << ",ok=" << (ok ? 1 : 0) << ",code=0x" << std::hex << code << std::dec
                        << ",playhead_ms=" << timeMs << '\n';
        }

        void ApplyGroups(Context& context, std::uint32_t timeMs)
        {
            const std::uint64_t wall = Wall();
            if (wall < g_nextGroupApplyWall)
                return;
            g_nextGroupApplyWall = wall + kGroupPollMs;
            const Track& track = context.clip.mixScenes;
            if (track.groups.empty())
                return;
            const std::uint32_t mainId = replay::census::RecordedPlayerId();
            const Ped live = PLAYER::PLAYER_PED_ID();
            for (std::uint32_t entityId : track.GroupedEntities())
            {
                const std::uint32_t recorded = track.GroupAt(entityId, timeMs);
                const replay::playback::Puppet* puppet = context.puppets.Find(replay::entity::ReplayId::FromPacked(entityId));
                if (puppet && puppet->clone)
                    ApplyGroup(context, puppet->clone, entityId, recorded, timeMs, "puppet");
                // The hidden player says the recorded player's lines
                // (SpeechPlayerOnPlayer): he carries the player's group too.
                if (entityId == mainId && live)
                    ApplyGroup(context, live, entityId, recorded, timeMs, "live_player");
            }
        }

        void RemoveGroups(Context& context)
        {
            for (const auto& entry : g_applied)
                if (replay::entities::Exists(entry.first))
                {
                    AUDIO::REMOVE_ENTITY_FROM_AUDIO_MIX_GROUP(entry.first, 0.0f);
                    ++g_groupRemoves;
                }
            if (!g_applied.empty())
                context.log << "MIX_GROUP,phase=play,action=remove_all,count=" << g_applied.size() << '\n';
            g_applied.clear();
        }
    }

    namespace
    {
        // ---- the hidden live player's state scenes ---------------------------
        // A wrapper driver's scene (mix_scene_pool_core.h kDrivers, Refusable)
        // that the clip recorded is the replay's while it owns the world: the
        // engine's instance (in the driver's reference) is stopped, the
        // driver's starts are refused by the start detour (it tests the
        // reference and tries again next frame) and the recorded instances are
        // started as the replay's own code scenes with their variables.
        std::vector<std::uint32_t> g_owned;
        std::vector<const pool::Driver*> g_ownedDrivers;
        bool g_ownActive = false;
        std::uint64_t g_ownStopsEngine = 0, g_refusedAtTake = 0;
        std::uint64_t g_ownStopsSkipped = 0; // the reference no longer named the scene
        std::uint64_t g_nextOwnLogWall = 0;
        const std::vector<std::uint32_t> kNeverReplay = {speechduck::kDuckScene, speechduck::kDuck2Scene};

        bool CanOwn()
        {
            return g_codeOk && replay::theatre::SceneStartInstalled() &&
                (replay::theatre::SceneStopInstalled() || PrologueAt(kMixerStop));
        }

        void StopEngineInstance(Context& context, const pool::Driver& d, const char* why, std::uint32_t timeMs)
        {
            std::uint64_t scene = 0;
            if (!replay::mem::Read(Base() + d.refRva, scene) || !scene)
                return;
            // The scene still names this reference (+0x310, written by the
            // start at 0x2F18F85 and zeroed by the stop 0x327A07C). The driver
            // stops its scene on its own thread and the pool reuses the slot: a
            // pointer read just before that would stop another scene, or one
            // already stopped (whose stop can start its settings' follow-up
            // scene again, 0x327A15B).
            std::uint64_t back = 0;
            if (!replay::mem::Read(static_cast<std::uintptr_t>(scene + pool::kSceneReference), back) ||
                back != static_cast<std::uint64_t>(Base() + d.refRva))
            {
                ++g_ownStopsSkipped;
                return;
            }
            std::uint32_t code = 0;
            const bool ok = CallMixerStop(scene, code);
            std::uint64_t after = 0;
            replay::mem::Read(Base() + d.refRva, after);
            ++g_ownStopsEngine;
            static replay::log::LogLimit againLogs;
            if (std::strcmp(why, "again") == 0 && !againLogs.Take(20))
                return;
            context.log << "MIX_SCENE_OWN,action=stop_engine,why=" << why << ",hash=0x" << std::hex << d.hash << ",ref=0x"
                        << d.refRva << ",scene=0x" << scene << std::dec << ",driver=" << d.name << ",result="
                        << (ok ? "ok" : "exception") << ",code=0x" << std::hex << code << std::dec << ",ref_after=0x" << std::hex
                        << after << std::dec << ",playhead_ms=" << timeMs << '\n';
        }

        void TakeOwnership(Context& context, std::uint32_t timeMs)
        {
            std::vector<std::uint32_t> rvas;
            std::string hashes;
            char item[24]{};
            for (const pool::Driver* d : g_ownedDrivers)
            {
                rvas.push_back(d->ReturnRva());
                std::snprintf(item, sizeof(item), "%s0x%x", hashes.empty() ? "" : "|", d->hash);
                hashes += item;
            }
            // The refusals first, then the stops (a driver on its own thread
            // could start its scene again between the stop and the refusal).
            replay::theatre::SetSceneRefusals(rvas);
            g_refusedAtTake = replay::theatre::SceneRefusedCount();
            for (const pool::Driver* d : g_ownedDrivers)
                StopEngineInstance(context, *d, "take", timeMs);
            g_ownActive = true;
            context.log << "MIX_SCENE_OWN,action=take,hashes=" << hashes << ",refusals=" << rvas.size() << ",playhead_ms=" << timeMs
                        << '\n';
        }

        void ReleaseOwnership(Context& context, std::uint32_t timeMs, const char* why)
        {
            if (!g_ownActive)
                return;
            replay::theatre::SetSceneRefusals({});
            g_ownActive = false;
            context.log << "MIX_SCENE_OWN,action=release,why=" << why << ",refused="
                        << (replay::theatre::SceneRefusedCount() - g_refusedAtTake) << ",stops_engine=" << g_ownStopsEngine
                        << ",playhead_ms=" << timeMs << '\n';
        }

        // While owning: the engine's instance came back (a start the detour
        // did not see) is stopped again; once a second the refusals so far.
        void KeepOwnership(Context& context, std::uint32_t timeMs)
        {
            for (const pool::Driver* d : g_ownedDrivers)
                StopEngineInstance(context, *d, "again", timeMs);
            const std::uint64_t wall = GetTickCount64();
            if (wall >= g_nextOwnLogWall)
            {
                g_nextOwnLogWall = wall + 1000;
                context.log << "MIX_SCENE_OWN,action=refuse,count=" << (replay::theatre::SceneRefusedCount() - g_refusedAtTake)
                            << ",playhead_ms=" << timeMs << '\n';
            }
        }

        std::string HashListText(const std::vector<std::uint32_t>& hashes)
        {
            std::string text;
            char item[16]{};
            for (std::uint32_t h : hashes)
            {
                std::snprintf(item, sizeof(item), "%s0x%x", text.empty() ? "" : "|", h);
                text += item;
            }
            return text.empty() ? std::string("none") : text;
        }
    }

    void Configure(const Config& config)
    {
        g_config = config;
        replay::theatre::SetSceneObserve(config.record || config.replay || config.probe);
    }

    const Config& GetConfig() { return g_config; }

    void Idle(Context& context)
    {
        if (!(g_config.record || g_config.replay || g_config.probe))
            return;
        Drain();
        // Nobody records or plays: the events only update the live set.
        if (!g_playing && Wall() - g_lastRecordWall > 500)
        {
            Process(context, nullptr, 0, "idle");
            // 4 times a second, a fresh scan of the pool after
            // the events: the first adopts the session's open scenes (the
            // starts before the hooks), then the drivers' direct ones; the
            // vanished check reads the same scan.
            static std::uint64_t nextIdleScan = 0;
            if (Wall() >= nextIdleScan)
            {
                nextIdleScan = Wall() + 250;
                if (g_config.poolRecord)
                    Pool(context.log, 0);
                Discover(context, nullptr, 0, "idle", !g_poolSynced);
                PollLive(context, nullptr, 0, "idle", false);
            }
        }
    }

    void Record(Context& context, std::uint32_t nowMs)
    {
        if (!(g_config.record || g_config.probe))
            return;
        g_lastRecordWall = Wall();
        Drain();
        Track* track = g_config.record ? &context.clip.mixScenes : nullptr;
        if (track && track->instances.empty())
        {
            g_varLast.clear(); // a fresh recording stores every variable again
            // MIXS v3: every scene the pool has now joins the
            // live set first (the recording starts with all of them).
            Discover(context, nullptr, nowMs, "record", true, 0); // a fresh scan
            const std::size_t added = TrackEnsureOpen(*track, g_live, nowMs);
            if (g_config.poolRecord && g_pool.ok)
                track->flags |= kTrackPoolScanned;
            if (added)
                context.log << "MIX_SCENE,phase=record,event=adopt_open,count=" << added << ",pool=" << (g_pool.ok ? 1 : 0)
                            << ",flags=" << track->flags << ",clip_ms=" << nowMs << '\n';
        }
        Process(context, track, nowMs, "record");
        // One fresh scan after the events, for both.
        if (g_config.poolRecord)
            Pool(context.log, 0);
        Discover(context, track, nowMs, "record", !g_poolSynced);
        PollLive(context, track, nowMs, "record", g_config.vars);
        if (track)
            PollGroups(context, *track, nowMs);
        PoolSnapshot(context, "rec", nowMs, false);
    }

    void BeginPlayback(Context& context)
    {
        g_playing = true;
        g_running.clear();
        g_applied.clear();
        g_playStarts = g_playStops = g_playFailed = g_groupAdds = g_groupRemoves = g_groupFailed = 0;
        g_varSets = g_varFailed = g_engineStopped = g_codeStarts = 0;
        g_attempts.clear();
        g_playLogged.clear();
        g_nextGroupApplyWall = 0;
        const Track& track = context.clip.mixScenes;
        std::size_t replayable = 0, code = 0, script = 0, conversation = 0;
        for (const Instance& instance : track.instances)
        {
            replayable += Replayable(instance.kind, g_config.kinds) ? 1 : 0;
            code += instance.kind == Kind::Code ? 1 : 0;
            script += instance.kind == Kind::Script ? 1 : 0;
            conversation += instance.kind == Kind::Conversation ? 1 : 0;
        }
        const bool calls = g_config.replay && CallsOk(context.log);
        // The scenes the replay owns (the clip recorded them). A refusal list
        // left by an earlier playback that did not reach its end goes with the
        // ownership it belonged to (otherwise a clip without those scenes would
        // keep the hidden player's driver refused while the theatre owns the
        // world).
        if (g_ownActive)
            replay::theatre::SetSceneRefusals({});
        g_owned.clear();
        g_ownedDrivers.clear();
        g_ownActive = false;
        g_ownStopsEngine = 0;
        g_ownStopsSkipped = 0;
        g_nextOwnLogWall = 0;
        g_nextPoolLogWall = 0;
        const bool poolOk = (g_config.engineOpenFromPool || g_config.ownPlayerState || g_config.poolLog) && PoolOk(context.log);
        if (calls && g_config.ownPlayerState && poolOk && CanOwn())
            for (const pool::Driver& d : pool::kDrivers)
            {
                if (!pool::Refusable(d) || !DriverOk(d))
                    continue;
                for (const Instance& instance : track.instances)
                    if (instance.hash == d.hash && instance.kind != Kind::Conversation)
                    {
                        g_owned.push_back(d.hash);
                        g_ownedDrivers.push_back(&d);
                        break;
                    }
            }
        std::string engineOpenText = OpenText();
        if (g_config.engineOpenFromPool && poolOk)
        {
            const PoolCache& p = Pool(context.log, 0);
            if (p.ok)
                engineOpenText = "pool:" + HashListText(pool::EngineOpen(p.scenes));
        }
        PoolSnapshot(context, "play_begin", 0, true);
        context.log << "MIX_SCENE_PLAYBACK,event=begin,replay=" << (g_config.replay ? 1 : 0) << ",calls=" << (calls ? 1 : 0)
                    << ",pool=" << (poolOk ? 1 : 0) << ",mixs_flags=" << track.flags << ",owned=" << HashListText(g_owned)
                    << ",can_own=" << (CanOwn() ? 1 : 0)
                    << ",kinds=" << static_cast<int>(g_config.kinds) << ",code_ok=" << (g_codeOk ? 1 : 0)
                    << ",vars=" << (g_config.vars ? 1 : 0) << ",vars_ok=" << (g_varsOk ? 1 : 0) << ",instances=" << track.instances.size()
                    << ",by_kind=code:" << code << "|script:" << script << "|conversation:" << conversation
                    << ",replayable=" << replayable << ",var_samples=" << track.vars.size() << ",code_skip=" << g_config.skip.size()
                    << ",groups=" << track.groups.size() << ",grouped_entities=" << track.GroupedEntities().size()
                    << ",lost_stops=" << track.lostStops << ",engine_open=" << engineOpenText << '\n';
    }

    void Play(Context& context, std::uint32_t timeMs)
    {
        if (g_config.record || g_config.replay || g_config.probe)
        {
            Drain();
            Process(context, nullptr, timeMs, "play");
            // 10 times a second, a fresh scan after the events and
            // the vanished check on it (the engine scenes that went leave the
            // live set; engineOpen itself comes from the scan).
            static std::uint64_t nextPlayScan = 0;
            if (Wall() >= nextPlayScan)
            {
                nextPlayScan = Wall() + 100;
                if (g_config.poolRecord || g_config.engineOpenFromPool)
                    Pool(context.log, 0);
                PollLive(context, nullptr, timeMs, "play", false);
            }
        }
        if (g_playing)
            PoolSnapshot(context, "play", timeMs, false);
        if (!g_config.replay || !g_playing || !CallsOk(context.log))
            return;
        const Track& track = context.clip.mixScenes;
        // The hidden live player's state scenes the clip has are the replay's
        // while it owns the world.
        if (!g_owned.empty())
        {
            const bool owning = replay::theatre::Owning() && CanOwn();
            if (owning && !g_ownActive)
                TakeOwnership(context, timeMs);
            else if (!owning && g_ownActive)
                ReleaseOwnership(context, timeMs, "not_owning");
            else if (g_ownActive)
                KeepOwnership(context, timeMs);
        }
        // A scene the replay started that the engine stopped
        // (vanished) leaves the running list; the plan starts it again, at
        // most kMaxAttempts times per instance.
        for (std::size_t n = g_running.size(); n-- > 0;)
        {
            Running& r = g_running[n];
            if (r.kind == Kind::Conversation || !r.scene || Alive(r.scene, r.hash))
                continue;
            ++g_engineStopped;
            context.log << "MIX_SCENE_PLAY,action=engine_stopped,id=" << r.id << ",hash=0x" << std::hex << r.hash << std::dec
                        << ",kind=" << KindName(r.kind) << ",attempts=" << g_attempts[r.id] << ",playhead_ms=" << timeMs << '\n';
            if (r.ref >= 0 && g_refs && !g_refs[r.ref])
                g_refUsed[r.ref] = false;
            g_running.erase(g_running.begin() + static_cast<std::ptrdiff_t>(n));
        }
        std::vector<std::uint32_t> engineOpen;
        bool fromPool = false;
        if (g_config.engineOpenFromPool)
        {
            // Every scene the engine has (not only the starts the mod saw: a
            // late hook warm-up misses the permanent scenes).
            const PoolCache& p = Pool(context.log, 100);
            if (p.ok)
            {
                // Without the replay's running scenes (a script scene it
                // started is in the pool with the script slot's reference;
                // counted as the engine's it would be stopped as "engine_owns"
                // on the next tick).
                engineOpen = pool::EngineOpen(p.scenes, RunningScenes());
                fromPool = true;
            }
        }
        if (!fromPool)
            for (const auto& entry : g_live.open)
                engineOpen.push_back(entry.second.hash);
        static const std::vector<std::uint32_t> kNone;
        std::vector<std::uint32_t> wanted = Wanted(track, timeMs, kMaxRunning, g_config.kinds, engineOpen, g_config.skip,
            g_ownActive ? g_owned : kNone, kNeverReplay);
        std::vector<std::uint32_t> running;
        for (const Running& r : g_running)
            running.push_back(r.id);
        // An instance that used its attempts is not started again.
        wanted.erase(std::remove_if(wanted.begin(), wanted.end(),
                         [&](std::uint32_t id) {
                             return std::find(running.begin(), running.end(), id) == running.end() && g_attempts[id] >= kMaxAttempts;
                         }),
            wanted.end());
        const Diff diff = Plan(wanted, running);
        for (std::uint32_t id : diff.stop)
            for (std::size_t n = 0; n < g_running.size(); ++n)
                if (g_running[n].id == id)
                {
                    const bool engineOwns = g_running[n].kind != Kind::Conversation &&
                        std::find(engineOpen.begin(), engineOpen.end(), g_running[n].hash) != engineOpen.end() &&
                        !(g_ownActive && std::find(g_owned.begin(), g_owned.end(), g_running[n].hash) != g_owned.end());
                    StopScene(context, g_running[n], timeMs, engineOwns ? "engine_owns" : "recorded_stop");
                    g_running.erase(g_running.begin() + static_cast<std::ptrdiff_t>(n));
                    break;
                }
        for (Running& r : g_running)
            ApplyVars(context, r, timeMs);
        // The groups before the scenes: a conversation scene's filter reads
        // the speakers' groups.
        ApplyGroups(context, timeMs);
        for (std::uint32_t id : diff.start)
            for (const Instance& instance : track.instances)
                if (instance.id == id)
                {
                    StartScene(context, instance, timeMs);
                    break;
                }
    }

    void EndPlayback(Context& context)
    {
        if (!g_playing)
            return;
        StopAll(context, static_cast<std::uint32_t>(context.transport.PlayheadMs()), "playback_end");
        // The driver starts the engine's own instance again by
        // the live state on its next frame.
        ReleaseOwnership(context, static_cast<std::uint32_t>(context.transport.PlayheadMs()), "playback_end");
        RemoveGroups(context);
        PoolSnapshot(context, "play_end", static_cast<std::uint32_t>(context.transport.PlayheadMs()), true);
        g_playing = false;
        context.log << "MIX_SCENE_PLAYBACK,event=end,replay=" << (g_config.replay ? 1 : 0) << ",started=" << g_playStarts
                    << ",stopped=" << g_playStops << ",failed=" << g_playFailed << ",group_adds=" << g_groupAdds
                    << ",group_removes=" << g_groupRemoves << ",group_failed=" << g_groupFailed << ",engine_starts=" << g_starts
                    << ",engine_stops=" << g_stops << ",lost_stops=" << g_lostStops << ",own_seen=" << g_mineSeen
                    << ",own_dropped=" << g_ownDropped << ",code_starts=" << g_codeStarts << ",engine_stopped=" << g_engineStopped
                    << ",var_sets=" << g_varSets << ",var_failed=" << g_varFailed << ",stale_stops=" << g_staleStops
                    << ",vanished=" << g_vanished << ",var_samples_recorded=" << g_varSamples
                    << ",by_kind=code:" << g_kindStarts[0] << "|script:" << g_kindStarts[1] << "|conversation:" << g_kindStarts[2]
                    << ",open_now=" << g_live.open.size() << ",claimed=" << g_claimed << ",discovered=" << g_discovered
                    << ",adopted=" << g_adopted << ",pool_scans=" << g_poolScans << ",owned=" << HashListText(g_owned)
                    << ",stops_engine=" << g_ownStopsEngine << ",stops_skipped=" << g_ownStopsSkipped << '\n';
    }

    std::size_t OpenCount() { return g_live.open.size() + g_running.size(); }

    bool ConversationActsOn(std::uint32_t group, std::uint32_t& sceneHash)
    {
        for (const auto& entry : g_live.open)
            if (entry.second.kind == Kind::Conversation && !Participant(entry.second, group))
            {
                sceneHash = entry.second.hash;
                return true;
            }
        for (const Running& running : g_running)
            if (running.kind == Kind::Conversation && !Participant(running.instance, group))
            {
                sceneHash = running.hash;
                return true;
            }
        return false;
    }

    std::string OpenText()
    {
        std::string text;
        char item[64]{};
        for (const auto& entry : g_live.open)
        {
            std::snprintf(item, sizeof(item), "%s0x%x:%s:tags=%u", text.empty() ? "" : "|", entry.second.hash,
                KindName(entry.second.kind), static_cast<unsigned>(entry.second.tagCount));
            text += item;
        }
        for (const Running& running : g_running)
        {
            std::snprintf(item, sizeof(item), "%s0x%x:%s:replay", text.empty() ? "" : "|", running.hash, KindName(running.kind));
            text += item;
        }
        return text.empty() ? std::string("none") : text;
    }
}
