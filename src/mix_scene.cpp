#include "mix_scene.h"
#include "engine_calls.h"
#include "engine_sigs.h"
#include "mix_scene_core.h"
#include "mix_scene_pool_core.h"
#include "speech_duck_core.h"

#include "entity_cache.h"
#include "game_sig.h"
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
// (mix_scene.h, mix_scene_core.h). Every internal function is found by
// signature (game_sig.h) with its prologue, most of them reached from the
// native body that calls them (the lea of its global and the call site), and
// called under SEH from the script thread. The signatures of the playback's
// calls (MixSceneReplay, gone) are still resolved and checked
// with the others; nothing calls them.

namespace replay::mixscene
{
    using enginesig::kConversationDuckStart;
    using enginesig::kConversationSceneCall;
    using enginesig::kConversationSceneStart;
    using enginesig::kMixerStartScene;
    using enginesig::kMixerStopScene;

    namespace
    {
        Config g_config;

        namespace sig = replay::sig;

        // ---- signatures and sites ----------------------------------------

        // The conversation duck's start (conversation): its only caller is the
        // manager's update; reads +0xE8, +0x30, +0x38 and the participants'
        // +0x7C/+0x98, nothing else of the conversation. Its load of the mixer
        // and its call of the mixer start (whose return tells a conversation
        // scene, mix_scene_core.h Callers).
        const sig::Signature kConversationMixerLea{"mix_conversation_start_mixer", kConversationDuckStart,
            sig::Offset{0x11E}, "48 8D 0D ?? ?? ?? ??", 0x2FC231E};
        const sig::Signature kConversationMixer{"mix_conversation_mixer", kConversationMixerLea, sig::Rel32{0x03, 0x07},
            "", 0x5B98760};
        // The conversation stop (conversation): stops +0xE8 (the mixer stop
        // (scene, 0)) and zeroes it.
        const sig::Signature kConversationStop{"mix_conversation_stop",
            "40 53 48 83 EC 20 48 8B D9 48 8B 89 E8 00 00 00 48 85 C9 74 0F 33 D2 E8 ?? ?? ?? ??", 0x2FC2964};
        const sig::Signature kConversationStopCall{"mix_conversation_stop_call", kConversationStop, sig::Offset{0x17},
            "E8 ?? ?? ?? ??", 0x2FC297B};
        // START_AUDIO_SCENE's handler (found by the native's hash, next to it in
        // the registration) calls the start by name, which loads the script
        // audio object and calls the script start (script audio, hash) -> bool;
        // STOP_AUDIO_SCENE's handler jumps to the stop by name, which calls the
        // script stop (script audio, hash): the calling script's scenes
        // (STOP_AUDIO_SCENES clears them at the theatre exit).
        const sig::Signature kStartAudioSceneNative{"mix_start_audio_scene_native",
            "48 8D 15 ?? ?? ?? ?? 48 B9 F2 E4 F6 08 FC 80 3A 01", 0xF810A8};
        const sig::Signature kStartAudioSceneHandler{"mix_start_audio_scene_handler", kStartAudioSceneNative,
            sig::Rel32{0x03, 0x07},
            "40 53 48 83 EC 20 48 8B 41 10 48 8B D9 48 8B 08 E8 ?? ?? ?? ?? 0F B6 D0 48 8B 03 89 10 48 83 C4 20 5B C3",
            0xF65208};
        const sig::Signature kStartAudioScene{"mix_start_audio_scene", kStartAudioSceneHandler, sig::Rel32{0x11, 0x15},
            "48 83 EC 28 33 C0 48 85 C9 74 15 33 D2 E8 ?? ?? ?? ?? 8B D0 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 83 C4 "
            "28 C3",
            0xF77B04};
        const sig::Signature kStartAudioSceneAudio{"mix_start_audio_scene_audio", kStartAudioScene, sig::Offset{0x14},
            "48 8D 0D ?? ?? ?? ??", 0xF77B18};
        const sig::Signature kStartAudioSceneObject{"mix_start_audio_scene_object", kStartAudioSceneAudio,
            sig::Rel32{0x03, 0x07}, "", 0x5C81C20};
        const sig::Signature kStartAudioSceneCall{"mix_start_audio_scene_call", kStartAudioScene, sig::Offset{0x1B},
            "E8 ?? ?? ?? ??", 0xF77B1F};
        const sig::Signature kScriptStart{"mix_script_start", kStartAudioSceneCall, sig::Rel32{0x01, 0x05},
            "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 ?? ?? ?? ?? 85 C0 79 04", 0x31C8910};
        const sig::Signature kStopAudioSceneNative{"mix_stop_audio_scene_native",
            "48 8D 15 ?? ?? ?? ?? 48 B9 88 E6 94 3B 2B 42 E8 DF", 0xF810F6};
        const sig::Signature kStopAudioSceneHandler{"mix_stop_audio_scene_handler", kStopAudioSceneNative,
            sig::Rel32{0x03, 0x07}, "48 8B 49 10 48 8B 09 E9 ?? ?? ?? ??", 0xF653C0};
        const sig::Signature kStopAudioScene{"mix_stop_audio_scene", kStopAudioSceneHandler, sig::Rel32{0x08, 0x0C},
            "48 83 EC 28 48 85 C9 74 15 33 D2 E8 ?? ?? ?? ?? 8B D0 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 83 C4 28 C3",
            0xF78220};
        const sig::Signature kStopAudioSceneAudio{"mix_stop_audio_scene_audio", kStopAudioScene, sig::Offset{0x12},
            "48 8D 0D ?? ?? ?? ??", 0xF78232};
        const sig::Signature kStopAudioSceneObject{"mix_stop_audio_scene_object", kStopAudioSceneAudio,
            sig::Rel32{0x03, 0x07}, "", 0x5C81C20};
        const sig::Signature kStopAudioSceneCall{"mix_stop_audio_scene_call", kStopAudioScene, sig::Offset{0x19},
            "E8 ?? ?? ?? ??", 0xF78239};
        const sig::Signature kScriptStop{"mix_script_stop", kStopAudioSceneCall, sig::Rel32{0x01, 0x05},
            "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 74", 0x31C946C};
        // The hash variants START (0xDC2F83A0612CA34D) and STOP
        // (0x6AB944DF68B512D3): their handlers load the script audio object and
        // call or jump to the same start and stop.
        const sig::Signature kStartSceneHashNative{"mix_start_scene_hash_native",
            "48 8D 15 ?? ?? ?? ?? 48 B9 4D A3 2C 61 A0 83 2F DC", 0xF810CF};
        const sig::Signature kStartSceneHash{"mix_start_scene_hash", kStartSceneHashNative, sig::Rel32{0x03, 0x07},
            "40 53 48 83 EC 20 48 8B 41 10 48 8B D9 48 8D 0D ?? ?? ?? ?? 8B 10 E8 ?? ?? ?? ?? 0F B6 D0 48 8B 03 89 10 "
            "48 83 C4 20 5B C3",
            0xF65254};
        const sig::Signature kStartSceneHashAudio{"mix_start_scene_hash_audio", kStartSceneHash, sig::Offset{0x0D},
            "48 8D 0D ?? ?? ?? ??", 0xF65261};
        const sig::Signature kStartSceneHashObject{"mix_start_scene_hash_object", kStartSceneHashAudio,
            sig::Rel32{0x03, 0x07}, "", 0x5C81C20};
        const sig::Signature kStartSceneHashCall{"mix_start_scene_hash_call", kStartSceneHash, sig::Offset{0x16},
            "E8 ?? ?? ?? ??", 0xF6526A};
        const sig::Signature kStartSceneHashTarget{"mix_start_scene_hash_target", kStartSceneHashCall,
            sig::Rel32{0x01, 0x05}, "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 ?? ?? ?? ?? 85 C0 79 04",
            0x31C8910};
        const sig::Signature kStopSceneHashNative{"mix_stop_scene_hash_native",
            "48 8D 15 ?? ?? ?? ?? 48 B9 D3 12 B5 68 DF 44 B9 6A", 0xF8111D};
        const sig::Signature kStopSceneHash{"mix_stop_scene_hash", kStopSceneHashNative, sig::Rel32{0x03, 0x07},
            "48 8B 41 10 48 8D 0D ?? ?? ?? ?? 8B 10 E9 ?? ?? ?? ??", 0xF653E4};
        const sig::Signature kStopSceneHashAudio{"mix_stop_scene_hash_audio", kStopSceneHash, sig::Offset{0x04},
            "48 8D 0D ?? ?? ?? ??", 0xF653E8};
        const sig::Signature kStopSceneHashObject{"mix_stop_scene_hash_object", kStopSceneHashAudio,
            sig::Rel32{0x03, 0x07}, "", 0x5C81C20};
        const sig::Signature kStopSceneHashJmp{"mix_stop_scene_hash_jmp", kStopSceneHash, sig::Offset{0x0D},
            "E9 ?? ?? ?? ??", 0xF653F1};
        const sig::Signature kStopSceneHashTarget{"mix_stop_scene_hash_target", kStopSceneHashJmp,
            sig::Rel32{0x01, 0x05}, "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 74",
            0x31C946C};
        // ADD_ENTITY_TO_AUDIO_MIX_GROUP's handler calls the group add (mixer,
        // entity, slot, hash, fade), which resolves the entity's audio itself
        // and refuses a hash with no mix group metadata. The native's fade: its
        // argument when above the floor, else the default.
        const sig::Signature kGroupNative{"mix_group_native",
            "85 C9 0F 84 ?? ?? ?? ?? 48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B F2", 0xF65E34};
        const sig::Signature kGroupNativeMixerLea{"mix_group_native_mixer", kGroupNative, sig::Offset{0x81},
            "48 8D 0D ?? ?? ?? ??", 0xF65EB5};
        const sig::Signature kGroupNativeMixer{"mix_group_mixer", kGroupNativeMixerLea, sig::Rel32{0x03, 0x07}, "",
            0x5B98760};
        const sig::Signature kFadeFloorRead{"mix_fade_floor_read", kGroupNative, sig::Offset{0x88},
            "0F 2F 35 ?? ?? ?? ??", 0xF65EBC};
        const sig::Signature kFadeFloor{"mix_fade_floor", kFadeFloorRead, sig::Rel32{0x03, 0x07}, "", 0x330F380};
        const sig::Signature kFadeDefaultRead{"mix_fade_default_read", kGroupNative, sig::Offset{0x9F},
            "F3 0F 10 05 ?? ?? ?? ??", 0xF65ED3};
        const sig::Signature kFadeDefault{"mix_fade_default", kFadeDefaultRead, sig::Rel32{0x04, 0x08}, "", 0x330F494};
        const sig::Signature kGroupNativeCall{"mix_group_native_call", kGroupNative, sig::Offset{0xAD},
            "E8 ?? ?? ?? ??", 0xF65EE1};
        const sig::Signature kGroupAdd{"mix_group_add", kGroupNativeCall, sig::Rel32{0x01, 0x05},
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 54 41 55 41 56 41 57 48 83 EC 40 4C 8B E1 0F 29 70 C8 "
            "48 8B CA 45 8B F0 41 8B F1",
            0x2EE52E8};
        // Its reads of the stack arguments (engine_calls.h): a build that
        // changed them fails here.
        const sig::Signature kGroupAddArg5{"mix_group_add_arg5", kGroupAdd, sig::Offset{0x102},
            "F3 0F 10 B4 24 90 00 00 00", 0x2EE53EA};
        // The code scenes' direct start (MixSceneReplayCode), as the weather and
        // ambient zones start theirs: the mixer start (mixer, &hash, &reference,
        // ...), then the release of the start's temporary list, which every
        // start site calls right after. START_AUDIO_SCENE's start in the script
        // thread's slot releases it too; its call of the mixer start is the
        // script scenes' start site (mix_scene_core.h Callers).
        const sig::Signature kZoneStart{"mix_zone_start",
            "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 60 8B 05 ?? ?? ?? ??", 0x31CEF28};
        const sig::Signature kZoneMixerLea{"mix_zone_start_mixer", kZoneStart, sig::Offset{0x63},
            "48 8D 0D ?? ?? ?? ??", 0x31CEF8B};
        const sig::Signature kZoneMixer{"mix_zone_mixer", kZoneMixerLea, sig::Rel32{0x03, 0x07}, "", 0x5B98760};
        const sig::Signature kZoneStartCall{"mix_zone_start_call", kZoneStart, sig::Offset{0xA7}, "E8 ?? ?? ?? ??",
            0x31CEFCF};
        const sig::Signature kZoneSceneStart{"mix_zone_scene_start", kZoneStartCall, sig::Rel32{0x01, 0x05},
            "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 18 41 56 48 83 EC 40 49 8B F0 4C 8B F2 8B 12 48 8B F9 41 B0 03 "
            "48 8D 0D ?? ?? ?? ?? 49 8B D9 E8 ?? ?? ?? ?? 4C 8B 94 24 80 00 00 00 4C 8B C6 44 8B 4C 24 70 48 8B D0 4C "
            "89 54 24 30 48 8B CF 4C 8B 54 24 78",
            0x2F19018};
        const sig::Signature kZoneTempReleaseCall{"mix_zone_temp_release", kZoneStart, sig::Offset{0xB1},
            "E8 ?? ?? ?? ??", 0x31CEFD9};
        const sig::Signature kTempRelease{"mix_temp_release", kZoneTempReleaseCall, sig::Rel32{0x01, 0x05},
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 45 33 F6 48 8B D9",
            0x2F03DAC};
        const sig::Signature kScriptSceneStart{"mix_script_scene_start",
            "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 60 8B EA", 0x31C8820};
        const sig::Signature kScriptStartCall{"mix_script_start_call", kScriptSceneStart, sig::Offset{0xC6},
            "E8 ?? ?? ?? ??", 0x31C88E6};
        const sig::Signature kScriptTempReleaseCall{"mix_script_temp_release", kScriptSceneStart, sig::Offset{0xD0},
            "E8 ?? ?? ?? ??", 0x31C88F0};
        const sig::Signature kScriptTempRelease{"mix_script_temp_release_target", kScriptTempReleaseCall,
            sig::Rel32{0x01, 0x05},
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 20 45 33 F6 48 8B D9",
            0x2F03DAC};
        // The scene's variable set (scene, name hash, value), as the player
        // state driver calls it (SET_AUDIO_SCENE_VARIABLE's by-name set jumps to
        // it).
        const sig::Signature kVarSetCall{"mix_var_set_call", pool::kPlayerStateDriver, sig::Offset{0x126},
            "E8 ?? ?? ?? ??", 0x2F2990A};
        const sig::Signature kVarSet{"mix_var_set", kVarSetCall, sig::Rel32{0x01, 0x05},
            "4C 8D 41 18 4D 63 50 50 49 8B C0 4F 8D 0C D0 4D 3B C1 74 0D 39 10 74 2B 48 83 C0 08 49 3B C1 75 F3 83 79 "
            "68 0A 7D 1B 41 8D 42 01 F3 0F 11 54 24 0C",
            0x3279628};

        // The RVA of the return address of the 5-byte call at `call` (what the
        // start detour reports as the caller), 0 when not found.
        std::uint32_t ReturnRva(std::uintptr_t call)
        {
            const std::uintptr_t base = sig::ImageBase();
            return call && base && call >= base ? static_cast<std::uint32_t>(call + 5 - base) : 0u;
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

        // ---- the scene pool -------------------------------------------------
        // mix_scene_pool_core.h: every scene the engine has, read directly
        // (rung 2, guarded copies) after the pool's code sites, the ready to
        // delete rule and the drivers' sites were found once.

        bool g_poolChecked = false;
        bool g_poolOk = false;
        std::string g_poolFailed = "none";
        bool g_driverOk[pool::kDriverCount] = {};
        // Per driver: its reference in the game (names a scene, also when the
        // driver is not ok, as the table did) and the RVA its start returns to
        // (the start detour's caller; 0 unless ok).
        std::uintptr_t g_driverRef[pool::kDriverCount] = {};
        std::uint32_t g_driverReturn[pool::kDriverCount] = {};
        // The three cells of the encoded pool pointer (valid byte, value, key).
        std::uintptr_t g_cellValid = 0, g_cellValue = 0, g_cellKey = 0;

        bool PoolOk(std::ostream& log)
        {
            if (g_poolChecked)
                return g_poolOk;
            g_poolChecked = true;
            for (const pool::CodeSite& site : pool::kSites)
                if (g_poolFailed == "none" && (!sig::Find(site.at) || (site.reach && !sig::Find(*site.reach))))
                    g_poolFailed = site.name;
            if (g_poolFailed == "none" && !sig::Find(pool::kReadyToDelete))
                g_poolFailed = "ready_to_delete";
            g_poolOk = g_poolFailed == "none";
            if (g_poolOk)
            {
                g_cellValid = sig::Find(pool::kPoolValidCell);
                g_cellValue = sig::Find(pool::kPoolValueCell);
                g_cellKey = sig::Find(pool::kPoolKeyCell);
            }
            std::string drivers;
            for (std::size_t n = 0; n < pool::kDriverCount; ++n)
            {
                const pool::Driver& d = pool::kDrivers[n];
                // The reference comes from the lea and the start from the call:
                // found means both instructions matched too.
                const std::uintptr_t reference = sig::Find(d.reference);
                const std::uintptr_t call = sig::Find(d.call);
                const std::uintptr_t start = sig::Find(d.via == pool::Via::Direct ? pool::kPoolStart : kMixerStartScene);
                g_driverOk[n] = g_poolOk && pool::DriverConsistent(d) && sig::Find(d.hashSite) && reference && call && start &&
                    sig::Find(d.start) == start;
                // A wrapper driver: its test of the reference after the start
                // reads that same reference.
                if (g_driverOk[n] && d.via == pool::Via::Wrapper)
                    g_driverOk[n] = pool::Refusable(d) && sig::Find(*d.tolerance) && sig::Find(*d.toleranceRef) == reference;
                g_driverRef[n] = reference;
                g_driverReturn[n] = g_driverOk[n] ? ReturnRva(call) : 0u;
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
            const std::size_t n = pool::DriverIndex(d);
            return n < pool::kDriverCount && g_driverOk[n];
        }
        std::uintptr_t DriverReference(const pool::Driver& d)
        {
            const std::size_t n = pool::DriverIndex(d);
            return n < pool::kDriverCount ? g_driverRef[n] : 0;
        }
        std::uint32_t DriverReturn(const pool::Driver& d)
        {
            const std::size_t n = pool::DriverIndex(d);
            return n < pool::kDriverCount ? g_driverReturn[n] : 0;
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

        bool ScanPool(PoolCache& out)
        {
            out.scenes.clear();
            out.slots = out.used = out.dying = 0;
            std::uint8_t valid = 0;
            std::uint64_t value = 0, key = 0;
            if (!g_cellValid || !g_cellValue || !g_cellKey || !replay::mem::Read(g_cellValid, valid) || !valid ||
                !replay::mem::Read(g_cellValue, value) || !replay::mem::Read(g_cellKey, key))
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
                pool::NameScene(s, pool::Dword(copy, pool::kSceneHash), pool::DriverByReference(s.reference, g_driverRef));
                if (s.driver && !DriverOk(*s.driver) && s.name == pool::Name::Direct)
                {
                    s.hash = 0; // a driver whose sites did not match does not name a scene
                    s.name = pool::Name::None;
                    s.driver = nullptr;
                }
                s.patches = pool::Patches(copy);
                s.hasApply = pool::VarOf(copy, pool::kApplyVar, s.apply);
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
                    s.driver ? s.driver->name : "-", s.replayOwned ? "replay" : "engine");
                if (text.size() < 3000)
                    text += item;
            }
            return text.empty() ? std::string("none") : text;
        }

        std::uint64_t g_nextPoolLogWall = 0;
        void PoolSnapshot(Context& context, const char* phase, std::uint32_t clipMs, bool force)
        {
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
        // A start from START_AUDIO_SCENE's site (a script scene).
        bool ScriptCaller(std::uint32_t caller) { return g_live.callers.script && caller == g_live.callers.script; }
        std::vector<std::uint32_t> g_playLogged; // hashes whose start this playback logged
        std::uint64_t g_staleStops = 0, g_vanished = 0, g_varSamples = 0;
        std::uint64_t g_eventCursor = 0;
        std::vector<replay::theatre::MixSceneEvent> g_pending;
        std::uint64_t g_lastRecordWall = 0;
        replay::log::LogLimit g_eventLogs;
        std::uint64_t g_starts = 0, g_stops = 0, g_lostStops = 0;
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
                    // A scene adopted from the pool before its start event was
                    // drained keeps its instance.
                    if (const std::uint32_t claimed = g_live.Claim(event.scene, event.hash, event.caller))
                    {
                        if (track)
                            if (Instance* adopted = track->Find(claimed))
                            {
                                adopted->caller = event.caller;
                                adopted->kind = KindOfCaller(event.caller, g_live.callers);
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
            const bool poolKnown = Pool(context.log, poolMaxAgeMs).ok;
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
            const PoolCache& p = Pool(context.log, maxAgeMs);
            if (!p.ok)
                return;
            for (const pool::PoolScene& s : p.scenes)
            {
                if (s.replayOwned || !s.hash || g_live.open.count(s.scene))
                    continue;
                std::uint32_t caller = 0;
                if (s.name == pool::Name::Direct && s.driver)
                    caller = DriverReturn(*s.driver);
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
        // The playback only follows the engine's scenes (logged); putting the
        // recorded ones back (MixSceneReplay) left with its keys.
        bool g_playing = false;

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
        // The start sites that tell a conversation or a script scene.
        g_live.callers.conversation = ReturnRva(sig::Find(kConversationSceneCall));
        g_live.callers.script = ReturnRva(sig::Find(kScriptStartCall));
        replay::theatre::SetSceneObserve(true); // the recording always reads the scenes
    }

    const Config& GetConfig() { return g_config; }

    void Idle(Context& context)
    {
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
                Pool(context.log, 0);
                Discover(context, nullptr, 0, "idle", !g_poolSynced);
                PollLive(context, nullptr, 0, "idle", false);
            }
        }
    }

    void Record(Context& context, std::uint32_t nowMs)
    {
        g_lastRecordWall = Wall();
        Drain();
        Track* track = &context.clip.mixScenes;
        if (track->instances.empty())
        {
            g_varLast.clear(); // a fresh recording stores every variable again
            // MIXS v3: every scene the pool has now joins the
            // live set first (the recording starts with all of them).
            Discover(context, nullptr, nowMs, "record", true, 0); // a fresh scan
            const std::size_t added = TrackEnsureOpen(*track, g_live, nowMs);
            if (g_pool.ok)
                track->flags |= kTrackPoolScanned;
            if (added)
                context.log << "MIX_SCENE,phase=record,event=adopt_open,count=" << added << ",pool=" << (g_pool.ok ? 1 : 0)
                            << ",flags=" << track->flags << ",clip_ms=" << nowMs << '\n';
        }
        Process(context, track, nowMs, "record");
        // One fresh scan after the events, for both.
        Pool(context.log, 0);
        Discover(context, track, nowMs, "record", !g_poolSynced);
        PollLive(context, track, nowMs, "record", true);
        PollGroups(context, *track, nowMs);
        PoolSnapshot(context, "rec", nowMs, false);
    }

    void BeginPlayback(Context& context)
    {
        g_playing = true;
        g_playLogged.clear();
        g_nextPoolLogWall = 0;
        const Track& track = context.clip.mixScenes;
        std::size_t code = 0, script = 0, conversation = 0;
        for (const Instance& instance : track.instances)
        {
            code += instance.kind == Kind::Code ? 1 : 0;
            script += instance.kind == Kind::Script ? 1 : 0;
            conversation += instance.kind == Kind::Conversation ? 1 : 0;
        }
        const bool poolOk = PoolOk(context.log);
        std::string engineOpenText = OpenText();
        if (poolOk)
        {
            const PoolCache& p = Pool(context.log, 0);
            if (p.ok)
                engineOpenText = "pool:" + HashListText(pool::EngineOpen(p.scenes));
        }
        PoolSnapshot(context, "play_begin", 0, true);
        context.log << "MIX_SCENE_PLAYBACK,event=begin,pool=" << (poolOk ? 1 : 0) << ",mixs_flags=" << track.flags
                    << ",instances=" << track.instances.size()
                    << ",by_kind=code:" << code << "|script:" << script << "|conversation:" << conversation
                    << ",var_samples=" << track.vars.size()
                    << ",groups=" << track.groups.size() << ",grouped_entities=" << track.GroupedEntities().size()
                    << ",lost_stops=" << track.lostStops << ",engine_open=" << engineOpenText << '\n';
    }

    void Play(Context& context, std::uint32_t timeMs)
    {
        Drain();
        Process(context, nullptr, timeMs, "play");
        // 10 times a second, a fresh scan after the events and
        // the vanished check on it (the engine scenes that went leave the
        // live set).
        static std::uint64_t nextPlayScan = 0;
        if (Wall() >= nextPlayScan)
        {
            nextPlayScan = Wall() + 100;
            Pool(context.log, 0);
            PollLive(context, nullptr, timeMs, "play", false);
        }
        if (g_playing)
            PoolSnapshot(context, "play", timeMs, false);
    }

    void EndPlayback(Context& context)
    {
        if (!g_playing)
            return;
        PoolSnapshot(context, "play_end", static_cast<std::uint32_t>(context.transport.PlayheadMs()), true);
        g_playing = false;
        context.log << "MIX_SCENE_PLAYBACK,event=end,engine_starts=" << g_starts << ",engine_stops=" << g_stops
                    << ",lost_stops=" << g_lostStops << ",stale_stops=" << g_staleStops << ",vanished=" << g_vanished
                    << ",var_samples_recorded=" << g_varSamples
                    << ",by_kind=code:" << g_kindStarts[0] << "|script:" << g_kindStarts[1] << "|conversation:" << g_kindStarts[2]
                    << ",open_now=" << g_live.open.size() << ",claimed=" << g_claimed << ",discovered=" << g_discovered
                    << ",adopted=" << g_adopted << ",pool_scans=" << g_poolScans << '\n';
    }

    std::size_t OpenCount() { return g_live.open.size(); }

    bool ConversationActsOn(std::uint32_t group, std::uint32_t& sceneHash)
    {
        for (const auto& entry : g_live.open)
            if (entry.second.kind == Kind::Conversation && !Participant(entry.second, group))
            {
                sceneHash = entry.second.hash;
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
        return text.empty() ? std::string("none") : text;
    }
}
