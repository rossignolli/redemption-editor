#pragma once

// The capture read probes 14 to 30, pure part (unit tested in
// tests/capture_probe_tests.cpp).
//
// Questions only a live session answers: what of the game camera beyond pose
// and FOV can be read, whether forcing wind, rain and snow would change the
// image, where lightning comes from, which timecycle modifiers and post
// effects a session uses, whether a wagon deforms, how many model swaps,
// explosions, glass hits, mix scenes, streams and music events there are, and
// so on. The probe counts and samples at ports proved in the 1491.50 dump and
// polls state by native and by guarded memory read; it changes nothing
// (capture_probe.cpp: every detour forwards its arguments and returns the
// original's result) and writes ReplayDebug\capture-probe-<date-time>.txt.
//
// This header holds what needs no engine: the probe, port and series tables,
// the per-mode statistics of a polled value, the small state machines
// (lightning, encrypted pool pointer, cloth handles) and the report text
// tools/capture_probe_report.py parses. The lock-free counters are the sound
// probe's (soundfx_probe_core.h).

#include "soundfx_probe_core.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace replay::captureprobe
{
    using soundfx::Histogram;
    using soundfx::KeyRow;
    using soundfx::KeyTable;
    using soundfx::kModes;
    using soundfx::Mode;
    using soundfx::ModeName;
    using soundfx::PortReport;
    using soundfx::PortStats;
    using soundfx::Hex;
    using soundfx::Number;
    using soundfx::PerSecond;

    // ------------------------------------------------------------ probes

    enum class Status : std::uint8_t
    {
        Implemented, // the question is answered by what the probe reads
        Partial,     // part of it; the note says what is left out
        Skipped,     // nothing installed; the note says why
    };
    inline const char* StatusName(Status status)
    {
        return status == Status::Implemented ? "implemented" : status == Status::Partial ? "partial" : "skipped";
    }

    struct ProbeInfo
    {
        int number;
        const char* name;
        Status status;
        const char* how;  // what is read
        const char* note; // what is left out and why ("" when nothing)
    };
    inline constexpr ProbeInfo kProbes[] = {
        {14, "game camera beyond pose and FOV", Status::Partial,
            "shake ports (by name and by metadata) with name, hash and amplitude; final against gameplay FOV and the camera state flags at 10 Hz",
            "depth of field and motion blur: no read port found in the dump (no getter native, no proven field); only when they are likely on (aim, cinematic, cutscene) is known"},
        {15, "wind, rain and snow in the recording and in the replay", Status::Implemented,
            "wind speed and direction, rain and snow by native and by the two globals and their overrides, at 30 Hz, kept per mode (idle, rec, play)",
            ""},
        {16, "where lightning comes from", Status::Implemented,
            "the coordinate port (script only) with its position; the lightning manager's current type, request flag and forced type every frame",
            ""},
        {17, "timecycle modifiers and post effects of a session", Status::Implemented,
            "modifier index, transition index and strength at 10 Hz; the modifier lookup by hash; AnimPostFX play and stop by hash",
            ""},
        {18, "water: what is simulation and what is shader", Status::Partial,
            "submerged level of the player, mount and vehicle, wave height (water height against the height without waves) at 4 Hz",
            "the image difference in the replay is judged by eye (the GPU water track was removed)"},
        {19, "which vehicle fields really change", Status::Implemented,
            "light state, dirt, mud, snow and wet level read from the vehicle (fields proved by their setter natives; there is no getter native), and speed, at 4 Hz",
            ""},
        {20, "does a wagon deform", Status::Implemented,
            "the damage port and the deformation port (impulse vector and offset) with the vehicle each call is about",
            "the before and after picture is taken by hand"},
        {21, "model hides and swaps", Status::Implemented,
            "the hide and swap port with model hashes, sphere and type; the active list's length at 2 Hz",
            ""},
        {22, "pickups left out", Status::Partial,
            "pickup pool count, pickups with an object and their models at 1 Hz",
            "the comparison with what the clip recorded is done by hand against the clip's object list"},
        {23, "explosion core as a single port", Status::Implemented,
            "the explosion port with type tag, position and owner, callers counted",
            "crossing with the EXPL track uses the clip's sound check report"},
        {24, "glass: who creates and who hits", Status::Partial,
            "the break-in-radius port (script and explosions) and the glass hit queue port with handle, crack type and position; the interface pointer the hit is reported to",
            "the function that creates the breakable glass was not proved in the dump: no detour on it"},
        {25, "cloth: a single place for the vertices", Status::Partial,
            "the pin frames port; cloth pieces of the player and mount (128 handles of the ped's variation object) and the class of each piece at 1 Hz",
            "no function that hands the vertices was found; vehicle cloth (canvas) is not read"},
        {26, "broken and switched off lights", Status::Implemented,
            "light intensity of every object within 40 m, the pool swept 300 natives per poll at 1 Hz and resumed where it stopped: lit count, lights that went dark on the same handle, lit objects whose handle vanished (a shot lamp becomes a fragment: new handle) with 32 samples, and the artificial lights byte",
            ""},
        {27, "what a cutscene needs to play as a recorded scene", Status::Partial,
            "the anim scene creation port with dictionary and playback list; anim scene camera, script camera, letterbox and control flags at 10 Hz",
            "cutscene lights and cutscene audio have no proven port"},
        {28, "mix scenes, zones, streams, banks and music per session", Status::Implemented,
            "13 ports: mixer scene start and stop (engine and script), scene variable, ambient zone, stream load, play and stop, script bank, music event",
            ""},
        {29, "RayFire objects through the inherited natives", Status::Implemented,
            "the composite entity pool read directly (the natives need a handle no enumeration gives): live objects, state per object name, state changes, at 2 Hz",
            "the animation phase is not read (it needs calls into the object)"},
        {30, "script screens through the TV and render target path", Status::Implemented,
            "the TV draw port, render target register (with the name) and link ports, and the TV channel global",
            ""},
    };
    constexpr int kFirstProbe = 14;
    constexpr int kLastProbe = 30;

    // ------------------------------------------------------------ ports

    enum Port : std::uint8_t
    {
        kShakeByName, kShake,
        kLightningAtCoords,
        kTimecycleLookup, kPostFxPlay, kPostFxStop,
        kVehicleDamage, kVehicleDeform,
        kMapChange,
        kExplosion,
        kGlassBreakRadius, kGlassHit,
        kClothPin,
        kAnimSceneCreate,
        kMixSceneStart, kMixSceneStop, kScriptSceneStart, kScriptSceneStop, kSceneVariable, kAmbientZone,
        kStreamLoad, kStreamPlayPosition, kStreamPlayPed, kStreamPlayFrontend, kStreamStop, kBankRequest, kMusicEvent,
        kTvDraw, kRenderTargetRegister, kRenderTargetLink,
        kPortCount,
    };

    struct PortInfo
    {
        Port port;
        int probe;
        const char* id;
        const char* name;
        std::uint32_t rva;     // build 1491.50
        const char* signature; // the port's first bytes, found once in the dump's code
        const char* label0;    // histogram 0 ("" = unused)
        const char* label1;
        const char* proof;     // how the role and the arguments were read in the dump
    };

    inline constexpr PortInfo kPorts[] = {
        {kShakeByName, 14, "C01", "camera_shake_by_name", 0x452328,
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 83 EC 50 48 8B 1D ?? ?? ?? ?? 48 8B E9 0F 29 70 E8",
            "amplitude", "", "SHAKE_GAMEPLAY_CAM body 0xF779C8 and 0xF73A00 (camera, name, amplitude in xmm2, count, int)"},
        {kShake, 14, "C02", "camera_shake", 0x45211C,
            "48 89 5C 24 08 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 30 0F B7 81 A8 02 00 00",
            "amplitude", "", "called by C01 with the metadata found by name, 11 callers (camera code): (camera, metadata with hash at +8, amplitude in xmm2, count, int)"},
        {kLightningAtCoords, 16, "C03", "lightning_at_coords", 0x141928C,
            "0F 2F 15 ?? ?? ?? ?? C7 81 E4 6D 00 00 03 00 00 00 C6 81 E1 6D 00 00 01",
            "x", "y", "_FORCE_LIGHTNING_FLASH_AT_COORDS body 0x104ABFC through 0x733E84, its only caller: (manager 0x4F2E0E0, position, float in xmm2)"},
        {kTimecycleLookup, 17, "C04", "timecycle_modifier_lookup", 0x2B2ED78,
            "40 53 48 83 EC 20 8B 02 48 8B D9 48 8D 54 24 30 89 44 24 30 E8 8B",
            "index", "", "SET_TIMECYCLE_MODIFIER body 0xFA2758 stores its result as the modifier index; 9 callers: (manager, hash) -> index or -1"},
        {kPostFxPlay, 17, "C05", "animpostfx_play", 0x7D4CA8,
            "48 89 5C 24 08 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 20 8B 02",
            "", "", "ANIMPOSTFX_PLAY body 0xF9CF54, 48 callers: (manager, hash, int, int, float, byte)"},
        {kPostFxStop, 17, "C06", "animpostfx_stop", 0x7D4ED8,
            "48 89 5C 24 08 48 89 6C 24 18 56 57 41 56 48 83 EC 20 8B 02 48 8B F2 48 8D 54 24 48 89 44 24 48 41 8B E8 48 8B F9 E8 E5",
            "", "", "ANIMPOSTFX_STOP body 0xFA34B4, 44 callers: (manager, hash, int)"},
        {kVehicleDamage, 20, "C07", "vehicle_damage", 0x156A2DC,
            "48 8B C4 44 89 48 20 4C 89 40 18 88 50 10 55 53 56 57 41 54",
            "damage", "", "SET_VEHICLE_DAMAGE body 0x10F3660 calls it on vehicle+0x3F0 with 18 arguments (the damage float is the 6th); 11 callers"},
        {kVehicleDeform, 20, "C08", "vehicle_deformation_impact", 0x1569924,
            "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 56 41 57 48 8D 68 98 48 81 EC 50 01 00 00 0F 29 70 D8 45 33 FF 44",
            "impulse_length", "offset_length", "SET_VEHICLE_DAMAGE body 0x10F3660 calls it on vehicle+0x400: (deformation, impulse vector, offset, entity, byte); 8 callers"},
        {kMapChange, 21, "C09", "model_hide_or_swap", 0xE2D77C,
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 54 41 55 41 56 41 57 48 83 EC 50 8B",
            "type", "radius", "CREATE_MODEL_HIDE body 0xF89B2C and CREATE_MODEL_SWAP body 0xF93BD8: (manager 0x4A46AA0, old hash, new hash, sphere, byte, type, byte, byte); 4 callers"},
        {kExplosion, 23, "C10", "explosion_core", 0x1116B5C,
            "48 8B C4 48 89 58 10 44 89 48 20 44 88 40 18 55 56 57 41",
            "tag", "", "ADD_EXPLOSION body 0xF905C4 and two more natives build the arguments with 0x6A775C (tag at +0, position at +0x10, owner at +0x28); 26 callers"},
        {kGlassBreakRadius, 24, "C11", "glass_break_in_radius", 0x6B3DE0,
            "48 8B C4 48 89 58 10 4C 89 48 20 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 E8 FE FF FF 48 81 EC E0 01 00 00 48",
            "radius", "damage", "BREAK_ENTITY_GLASS body 0xFCBD54, one more native and the explosion code: (entity, position, radius in xmm2, impulse, damage, int, byte)"},
        {kGlassHit, 24, "C12", "glass_hit", 0x299DF48,
            "48 8B C4 48 89 58 08 48 89 68 18 48 89 70 20 89 50 10 57 41 54 41 55 41 56 41 57 48 83 EC 40 0F B7",
            "crack_type", "impulse_length", "called by C11 per glass pane with (handle, crack type, position, impulse); queues at most 20 hits; 3 callers"},
        {kClothPin, 25, "C13", "cloth_pin_frames", 0x8DCE70,
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 40 8A F2 48 8D 99 C8 04 00 00 BF 80 00 00 00 83 3B FF 74 1F",
            "pieces", "frames", "SET_PED_CLOTH_PIN_FRAMES body 0xFEA190: walks 128 cloth handles at +0x4C8 of the ped's variation object; 5 callers"},
        {kAnimSceneCreate, 27, "C14", "anim_scene_create", 0x2FD709C,
            "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 40 8B 19",
            "", "", "_CREATE_ANIM_SCENE body 0xF6A230: (dictionary hash, flags, playback list hash, byte) -> scene handle; 9 callers"},
        {kMixSceneStart, 28, "C15", "mixer_scene_start", 0x2F19018,
            "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 40 49 8B F0",
            "", "", "the script scene start 0x31C8820 calls it with the scene hash; it writes the hash at scene+0x328; 64 callers"},
        {kMixSceneStop, 28, "C16", "mixer_scene_stop", 0x327A07C,
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 70 0F 29 70 E8 48 8D 79",
            "", "", "STOP_AUDIO_SCENE's function 0x31C946C calls it with the scene found by hash; 97 callers: (scene, byte)"},
        {kScriptSceneStart, 28, "C17", "script_audio_scene_start", 0x31C8910,
            "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 F0 23",
            "accepted", "", "START_AUDIO_SCENE body 0xF77B04 and a second native: (script audio 0x5C81C20, scene hash) -> bool"},
        {kScriptSceneStop, 28, "C18", "script_audio_scene_stop", 0x31C946C,
            "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 90 C5",
            "", "", "STOP_AUDIO_SCENE body 0xF78220: (script audio, scene hash)"},
        {kSceneVariable, 28, "C19", "audio_scene_variable", 0x31C6684,
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 0F 29 74 24 20 41 8B F8 0F 28 F3 8B F2",
            "value", "", "SET_AUDIO_SCENE_VARIABLE through 0x31C66EC: (script audio, scene hash, variable hash, value in xmm3)"},
        {kAmbientZone, 28, "C20", "ambient_zone_state", 0x31C60BC,
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 30 45 8A E1 41",
            "enabled", "", "SET_AMBIENT_ZONE_STATE body 0xF73F98 and the list variant 0xF73E30: (script audio, zone hash, enabled, force)"},
        {kStreamLoad, 28, "C21", "stream_load", 0x31C409C,
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 50 33 DB 49 8B F1",
            "accepted", "", "LOAD_STREAM body 0xF72D00 and a second native: (script audio, stream hash, start offset, set hash, byte, int) -> bool; 3 callers"},
        {kStreamPlayPosition, 28, "C22", "stream_play_from_position", 0x31C3320,
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 41 8B F8 48 8B F2",
            "x", "y", "PLAY_STREAM_FROM_POSITION body 0xF72ADC: (script audio, position, stream id)"},
        {kStreamPlayPed, 28, "C23", "stream_play_from_ped", 0x31C30AC,
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 8B F8 48 8B F2 48 8B D9 E8 4B",
            "", "", "PLAY_STREAM_FROM_PED body 0xF72A74: (script audio, ped, stream id)"},
        {kStreamPlayFrontend, 28, "C24", "stream_play_frontend", 0x31C34E0,
            "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 E8 20 78",
            "", "", "PLAY_STREAM_FRONTEND body 0xF72B3C: (script audio, stream id)"},
        {kStreamStop, 28, "C25", "stream_stop", 0x31C9878,
            "48 89 5C 24 08 57 48 83 EC 20 41 83 C8 FF 48 8B D9",
            "", "", "STOP_STREAM body 0xF786C4: (script audio, stream id)"},
        {kBankRequest, 28, "C26", "script_audio_bank_request", 0x31C57F0,
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 41 8A F0 49",
            "accepted", "", "REQUEST_SCRIPT_AUDIO_BANK body 0xF736D8: (script audio, bank name, byte, vector, float, pointer) -> bool"},
        {kMusicEvent, 28, "C27", "music_event", 0x3173354,
            "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 49 8B F8 41",
            "from_script", "", "TRIGGER_MUSIC_EVENT body 0xF79250 and the hash variant: (music manager, event hash, pointer, script id); 16 callers"},
        {kTvDraw, 30, "C28", "tv_channel_draw", 0xF95214,
            "48 81 EC 98 00 00 00 48 8B 05 ?? ?? ?? ?? 0F 28 E8 48 85 C0",
            "width", "height", "DRAW_TV_CHANNEL handler 0xF8B1D4, its only caller: (x, y, width, height in xmm0 to xmm3, rotation, r, g, b, a)"},
        {kRenderTargetRegister, 30, "C29", "render_target_register", 0x1056800,
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 40 8A F2 48 8B D9 E8 F6",
            "accepted", "", "REGISTER_NAMED_RENDERTARGET handler 0x1046C2C, its only caller: (name, bool) -> bool"},
        {kRenderTargetLink, 30, "C30", "render_target_link", 0x1055280,
            "48 8B C4 48 89 58 08 57 48 83 EC 20 48 8D 50 18 C7 40 10 FF",
            "", "", "LINK_NAMED_RENDERTARGET handler 0x1045EB4 jumps to it: (model hash)"},
    };
    static_assert(sizeof(kPorts) / sizeof(kPorts[0]) == kPortCount, "one row per port, in the enum's order");

    inline const PortInfo& Info(Port port) { return kPorts[port < kPortCount ? port : 0]; }

    // What the dump did not prove, or what no port reads: listed in the
    // report as skipped or indirect, with the reason.
    struct SkippedInfo
    {
        int probe;
        const char* what;
        const char* kind; // "skipped": not measured; "indirect": read another way
        const char* reason;
    };
    inline constexpr SkippedInfo kSkipped[] = {
        {14, "depth of field and motion blur of the game camera", "skipped",
            "no getter native and no field proved in the dump; the camera state flags say when they are likely on"},
        {16, "FORCE_LIGHTNING_FLASH (0x733E7C)", "indirect",
            "an 8 byte function that only sets the manager's request flag: no detour; the flag and the lightning type are read every frame"},
        {16, "natural lightning (the weather's own)", "indirect",
            "started inside the manager's update 0x1459B6C, not through a single call: seen as the manager's type field 0x4F2E0E0+0x6E00 leaving zero"},
        {19, "vehicle light, dirt, mud, snow and wet by native", "indirect",
            "inc/natives.h has setters only; the fields the setters write are read from the vehicle (+0x9F0, +0x99C, +0x9A0, +0x98C, +0x988)"},
        {22, "pickups the clip left out", "indirect",
            "the pool is counted; the clip's own list is compared by hand"},
        {24, "breakable glass creation", "skipped",
            "the creating function was not proved in the dump (candidates 0x299D040 and 0x299E148 have one or two callers and no native path)"},
        {24, "glass handle lookup 0x299D1F0", "skipped",
            "a 24 byte leaf with 16 callers that only maps a handle to its glass: nothing to learn from a count"},
        {25, "cloth vertices", "skipped",
            "no function that hands the vertices was found; the cloth pieces are counted and their class read"},
        {25, "cloth piece lookup 0x2D1F230", "indirect",
            "a leaf lookup (manager, handle); the probe reads the same table itself"},
        {26, "broken map light", "indirect",
            "no port installed; a lit object whose handle vanishes between two complete sweeps (the broken lamp: a fragment with a new handle) is counted beside a light whose intensity goes to zero while its object stays (LIGHT_VANISHED samples); candidate ports found in the dump and not installed: 0x6E38AC (delegate that creates the broken fragment's object: rcx = new fragInst; its invoker is outside the dump) and 0xE482D4 (break effects of a CObject child: rcx = object; edx = child; reaches the light entity through 0x12A0B20 and 0x633CB4, which sets bit 13 of light+0xB0)"},
        {27, "cutscene lights and cutscene audio", "skipped",
            "no port found in the dump for either"},
        {28, "script wave bank load (LOAD_SCRIPT_WAVE_BANK of the GTA)", "skipped",
            "no native of that role in inc/natives.h; only REQUEST_SCRIPT_AUDIO_BANK is probed"},
        {29, "RayFire state by GET_STATE_OF_RAYFIRE_MAP_OBJECT", "indirect",
            "the native takes a pool handle nothing enumerates; the pool and the state field +0x264 it reads are read directly"},
        {29, "RayFire animation phase", "skipped",
            "GET_RAYFIRE_MAP_OBJECT_ANIM_PHASE calls into the object; not done from a poll"},
        {30, "DRAW_SPRITE and the movie view render target", "skipped",
            "not asked by the probe; their handlers are 0xF89294 and 0x109B20C"},
    };

    // ------------------------------------------------------------ polled series

    enum Series : std::uint8_t
    {
        // 14
        kFinalFov, kGameplayFov, kFovDelta, kCamShaking, kAimCam, kFirstPerson, kCinematicCam, kCinematicMode,
        kScreenFading, kScreenFadedOut,
        // 15
        kWindSpeed, kWindDirX, kWindDirY, kRainNative, kSnowNative, kRainGlobal, kSnowGlobal, kRainOverride,
        kSnowOverride,
        // 16
        kLightningType, kLightningRequest, kLightningForcedType,
        // 17
        kTimecycleIndex, kTimecycleTransitionIndex, kTimecycleStrength,
        // 18
        kPlayerSubmerged, kMountSubmerged, kVehicleSubmerged, kWaveHeight,
        // 19
        kVehicleLightState, kVehicleDirt, kVehicleMud, kVehicleSnow, kVehicleWet, kVehicleSpeed,
        // 21
        kMapChangesActive,
        // 22
        kPickupCount, kPickupObjects,
        // 24
        kGlassInterface,
        // 25
        kPlayerClothPieces, kMountClothPieces,
        // 26
        kLightsNear, kLightsLit, kArtificialLights, kLightsPool, kLightSweepNatives, kLightSweepPolls,
        // 27
        kAnimSceneCam, kScriptCam, kPlayerInAnimScene, kLetterbox, kControlOff,
        // 29
        kRayfireLive, kRayfireAnimating,
        // 30
        kTvChannel,
        kSeriesCount,
    };

    struct SeriesInfo
    {
        Series series;
        int probe;
        const char* name;
        const char* source; // native, or the address read and how it was found
    };

    inline constexpr SeriesInfo kSeries[] = {
        {kFinalFov, 14, "final_fov", "GET_FINAL_RENDERED_CAM_FOV"},
        {kGameplayFov, 14, "gameplay_fov", "GET_GAMEPLAY_CAM_FOV"},
        {kFovDelta, 14, "final_minus_gameplay_fov", "the two natives"},
        {kCamShaking, 14, "gameplay_cam_shaking", "IS_GAMEPLAY_CAM_SHAKING"},
        {kAimCam, 14, "aim_cam", "IS_AIM_CAM_ACTIVE"},
        {kFirstPerson, 14, "first_person", "_IS_IN_FULL_FIRST_PERSON_MODE or IS_FIRST_PERSON_AIM_CAM_ACTIVE"},
        {kCinematicCam, 14, "cinematic_cam", "IS_CINEMATIC_CAM_RENDERING"},
        {kCinematicMode, 14, "cinematic_mode", "_IS_IN_CINEMATIC_MODE"},
        {kScreenFading, 14, "screen_fading", "IS_SCREEN_FADING_OUT or IS_SCREEN_FADING_IN"},
        {kScreenFadedOut, 14, "screen_faded_out", "IS_SCREEN_FADED_OUT"},
        {kWindSpeed, 15, "wind_speed", "GET_WIND_SPEED"},
        {kWindDirX, 15, "wind_direction_x", "GET_WIND_DIRECTION"},
        {kWindDirY, 15, "wind_direction_y", "GET_WIND_DIRECTION"},
        {kRainNative, 15, "rain_level", "GET_RAIN_LEVEL"},
        {kSnowNative, 15, "snow_level", "GET_SNOW_LEVEL"},
        {kRainGlobal, 15, "rain_global", "0x4576250 (GET_RAIN_LEVEL reads it)"},
        {kSnowGlobal, 15, "snow_global", "0x4576254 (GET_SNOW_LEVEL reads it)"},
        {kRainOverride, 15, "rain_override", "0x4576440 (SET_RAIN writes it)"},
        {kSnowOverride, 15, "snow_override", "0x4576444 (_SET_SNOW_LEVEL writes it)"},
        {kLightningType, 16, "lightning_type", "0x4F2E0E0+0x6E00 (0 none, 1 directional, 2 cloud, 3 strike)"},
        {kLightningRequest, 16, "lightning_request", "0x4F2E0E0+0x6DE0 (FORCE_LIGHTNING_FLASH writes 1)"},
        {kLightningForcedType, 16, "lightning_forced_type", "0x4F2E0E0+0x6DE4 (the coordinate port writes 3)"},
        {kTimecycleIndex, 17, "timecycle_modifier_index", "GET_TIMECYCLE_MODIFIER_INDEX"},
        {kTimecycleTransitionIndex, 17, "timecycle_transition_index", "GET_TIMECYCLE_TRANSITION_MODIFIER_INDEX"},
        {kTimecycleStrength, 17, "timecycle_modifier_strength", "0x4A995F4 (SET_TIMECYCLE_MODIFIER_STRENGTH writes it)"},
        {kPlayerSubmerged, 18, "player_submerged", "GET_ENTITY_SUBMERGED_LEVEL"},
        {kMountSubmerged, 18, "mount_submerged", "GET_ENTITY_SUBMERGED_LEVEL"},
        {kVehicleSubmerged, 18, "vehicle_submerged", "GET_ENTITY_SUBMERGED_LEVEL"},
        {kWaveHeight, 18, "wave_height", "GET_WATER_HEIGHT minus GET_WATER_HEIGHT_NO_WAVES at the player"},
        {kVehicleLightState, 19, "vehicle_light_state", "vehicle+0x9F0 low 3 bits (SET_VEHICLE_LIGHTS writes them)"},
        {kVehicleDirt, 19, "vehicle_dirt", "vehicle+0x99C (SET_VEHICLE_DIRT_LEVEL writes it)"},
        {kVehicleMud, 19, "vehicle_mud", "vehicle+0x9A0 (_SET_VEHICLE_MUD_LEVEL writes it)"},
        {kVehicleSnow, 19, "vehicle_snow", "vehicle+0x98C (_SET_VEHICLE_SNOW_LEVEL writes it)"},
        {kVehicleWet, 19, "vehicle_wet", "vehicle+0x988 (_SET_VEHICLE_WET_LEVEL writes it)"},
        {kVehicleSpeed, 19, "vehicle_speed", "GET_ENTITY_SPEED"},
        {kMapChangesActive, 21, "map_changes_active", "0x4A46AA0+8 (the hide and swap list's length)"},
        {kPickupCount, 22, "pickups_in_pool", "worldGetAllPickups"},
        {kPickupObjects, 22, "pickups_with_object", "GET_PICKUP_OBJECT"},
        {kGlassInterface, 24, "glass_hit_interface_present", "0x5995D18 (the object the break-in-radius port reports each hit to)"},
        {kPlayerClothPieces, 25, "player_cloth_pieces", "the 128 handles at +0x4C8 of the ped's variation object"},
        {kMountClothPieces, 25, "mount_cloth_pieces", "the same, on the mount"},
        {kLightsNear, 26, "objects_checked_for_light", "worldGetAllObjects within 40 m, per complete sweep (no cap)"},
        {kLightsLit, 26, "objects_lit", "_GET_OBJECT_LIGHT_INTENSITY above zero, per complete sweep"},
        {kArtificialLights, 26, "artificial_lights_off", "0x3F0D6BB (SET_ARTIFICIAL_LIGHTS_STATE writes it)"},
        {kLightsPool, 26, "objects_in_pool", "worldGetAllObjects count at the start of a sweep (2048 at most)"},
        {kLightSweepNatives, 26, "light_sweep_natives", "natives one complete sweep cost (300 per poll, resumed)"},
        {kLightSweepPolls, 26, "light_sweep_polls", "polls one complete sweep took (1 Hz)"},
        {kAnimSceneCam, 27, "anim_scene_cam", "_IS_ANIM_SCENE_CAM_ACTIVE"},
        {kScriptCam, 27, "script_cam", "the gameplay camera is not the rendering one"},
        {kPlayerInAnimScene, 27, "player_in_anim_scene", "IS_ENTITY_PLAYING_ANIM_SCENE"},
        {kLetterbox, 27, "letterbox", "HAS_LETTER_BOX"},
        {kControlOff, 27, "player_control_off", "IS_PLAYER_CONTROL_ON"},
        {kRayfireLive, 29, "rayfire_objects", "the composite entity pool (0x4A3E810, the pool GET_STATE_OF_RAYFIRE_MAP_OBJECT reads)"},
        {kRayfireAnimating, 29, "rayfire_animating", "pool objects whose state +0x264 is 7"},
        {kTvChannel, 30, "tv_channel", "0x4C389C0 (GET_TV_CHANNEL reads it)"},
    };
    static_assert(sizeof(kSeries) / sizeof(kSeries[0]) == kSeriesCount, "one row per series, in the enum's order");

    inline const SeriesInfo& Info(Series series) { return kSeries[series < kSeriesCount ? series : 0]; }

    // A polled value per mode: range, mean, how often it changed and how
    // many polls could not read it. Script thread only.
    struct SeriesStats
    {
        Histogram byMode[kModes];
        std::uint64_t changes[kModes] = {};
        std::uint64_t unread = 0;
        double last = 0.0;
        bool hasLast = false;

        // A change: more than `epsilon` away from the previous sample.
        void Add(std::size_t mode, double value, double epsilon = 1e-4)
        {
            if (mode >= kModes)
                mode = 0;
            if (!std::isfinite(value))
            {
                ++unread;
                return;
            }
            if (hasLast && std::fabs(value - last) > epsilon)
                ++changes[mode];
            last = value;
            hasLast = true;
            byMode[mode].Add(value);
        }

        void Unread() { ++unread; }

        std::uint64_t Samples() const { return byMode[0].count + byMode[1].count + byMode[2].count; }
    };

    // ------------------------------------------------------------ small state machines

    // Probe 16: the lightning manager's current type, seen once per frame.
    // A lightning is counted when the type leaves zero or changes to
    // another non-zero type.
    struct LightningTracker
    {
        static constexpr int kTypes = 4; // 0 none, 1 directional, 2 cloud, 3 strike
        std::uint64_t started[kTypes] = {};
        std::uint64_t otherType = 0;      // a type outside 1..3
        std::uint64_t requestFrames = 0;  // frames with the request flag set
        std::uint64_t requestRises = 0;
        std::uint64_t forcedFrames = 0;   // frames with a forced type
        int lastType = 0;
        bool lastRequest = false;

        // True when this frame starts a lightning.
        bool Step(int type, bool request, int forcedType)
        {
            if (request)
            {
                ++requestFrames;
                if (!lastRequest)
                    ++requestRises;
            }
            lastRequest = request;
            if (forcedType > 0)
                ++forcedFrames;
            const bool start = type != 0 && type != lastType;
            if (start)
            {
                if (type > 0 && type < kTypes)
                    ++started[type];
                else
                    ++otherType;
            }
            lastType = type;
            return start;
        }

        std::uint64_t Total() const { return started[1] + started[2] + started[3] + otherType; }
    };

    // Probe 26: one object within the light radius, as a sweep saw it. The model
    // is read for lit objects only (one native less per unlit object).
    struct LightSeen
    {
        int handle = 0;
        std::uint32_t model = 0;
        float intensity = 0.0f;
        float xyz[3] = {};
    };

    // A lit object the next complete sweep did not see: its last state,
    // whether the handle still exists (a shot lamp's handle does not: the
    // game replaces it with a fragment object), its distance to the player
    // when it does (it left the radius), and the nearest object that
    // appeared in the same sweep within kNewObjectRadius (the fragment).
    struct LitVanishedSample
    {
        std::uint32_t model = 0;
        float xyz[3] = {};
        float intensity = 0.0f;
        bool exists = false;
        float distance = -1.0f;    // to the player, when the handle still exists
        std::uint32_t newModel = 0; // 0 when nothing appeared within kNewObjectRadius
        float newDistance = -1.0f;
    };

    // Probe 26: the per-handle light state across sweeps. A sweep is one walk over
    // the whole pool (it may span several polls, LightSweep); the comparison is
    // always between two complete sweeps. A shot lamp becomes a fragment with a
    // new handle, so a lit object the next complete sweep does not see is counted
    // as vanished. Script thread only.
    struct LightTracker
    {
        static constexpr std::size_t kSamples = 32;
        static constexpr float kNewObjectRadius = 2.0f;

        std::uint64_t wentDark = 0;     // same handle, intensity fell to zero
        std::uint64_t cameOn = 0;       // same handle, intensity rose from zero
        std::uint64_t litVanished = 0;  // lit in the previous sweep, not seen in this one
        std::uint64_t vanishedGone = 0; // of those: the handle does not exist any more
        std::uint64_t vanishedLeft = 0; // of those: the handle exists (left the radius or the pool list)
        std::uint64_t sweeps = 0;       // complete sweeps
        std::vector<LitVanishedSample> samples;
        std::unordered_map<int, LightSeen> last; // the previous complete sweep
        std::unordered_map<int, LightSeen> seen; // the sweep in progress

        // An object within the radius, its intensity read.
        void Note(const LightSeen& object)
        {
            const auto known = last.find(object.handle);
            if (object.intensity > 0.0f)
            {
                if (known != last.end() && known->second.intensity <= 0.0f)
                    ++cameOn;
            }
            else if (known != last.end() && known->second.intensity > 0.0f)
                ++wentDark;
            seen[object.handle] = object;
        }

        // The lit objects of the previous sweep this sweep did not see, once
        // the sweep is complete and before EndSweep.
        std::vector<LightSeen> Vanished() const
        {
            std::vector<LightSeen> out;
            for (const auto& entry : last)
                if (entry.second.intensity > 0.0f && seen.find(entry.first) == seen.end())
                    out.push_back(entry.second);
            std::sort(out.begin(), out.end(), [](const LightSeen& a, const LightSeen& b) { return a.handle < b.handle; });
            return out;
        }

        // The nearest object this sweep saw that the previous one did not,
        // within kNewObjectRadius of `xyz`: its handle, 0 when none.
        int NearestNew(const float* xyz, float& distance) const
        {
            int best = 0;
            distance = -1.0f;
            for (const auto& entry : seen)
            {
                if (last.find(entry.first) != last.end())
                    continue;
                const float dx = entry.second.xyz[0] - xyz[0];
                const float dy = entry.second.xyz[1] - xyz[1];
                const float dz = entry.second.xyz[2] - xyz[2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d <= kNewObjectRadius && (best == 0 || d < distance || (d == distance && entry.first < best)))
                {
                    best = entry.first;
                    distance = d;
                }
            }
            return best;
        }

        // Counts one vanished lit object. True when a sample is still wanted
        // (the caller fills the fragment's model and calls AddSample).
        bool Vanish(const LightSeen& old, bool exists)
        {
            (void)old;
            ++litVanished;
            if (exists)
                ++vanishedLeft;
            else
                ++vanishedGone;
            return samples.size() < kSamples;
        }

        void AddSample(const LitVanishedSample& sample)
        {
            if (samples.size() < kSamples)
                samples.push_back(sample);
        }

        void EndSweep()
        {
            last.swap(seen);
            seen.clear();
            ++sweeps;
        }
    };

    // Probe 26: the pool's object list, taken once per sweep and walked over as
    // many polls as the native budget allows (a fixed cap in pool order could miss
    // the lamp in front of the player). A poll spends kNativeBudget natives at
    // most on the walk; the next poll resumes at `cursor`.
    struct LightSweep
    {
        static constexpr int kCapacity = 2048;
        static constexpr int kNativeBudget = 300;
        static constexpr int kWorstPerObject = 3; // coords, intensity, model

        int handles[kCapacity] = {};
        int count = 0;
        int cursor = 0;
        bool active = false;
        int checked = 0;     // within the radius, this sweep
        int lit = 0;
        int natives = 0;     // this sweep's cost
        int polls = 0;       // polls this sweep took
        int pollNatives = 0; // spent in the current poll

        void Begin(int objects)
        {
            count = objects < 0 ? 0 : objects > kCapacity ? kCapacity : objects;
            cursor = 0;
            active = true;
            checked = lit = natives = polls = pollNatives = 0;
        }
        void NewPoll()
        {
            pollNatives = 0;
            ++polls;
        }
        bool HasBudget() const { return pollNatives + kWorstPerObject <= kNativeBudget; }
        void Spend(int n)
        {
            pollNatives += n;
            natives += n;
        }
        bool Walking() const { return active && cursor < count; }
        bool Done() const { return active && cursor >= count; }
        void End() { active = false; }
    };

    // Probe 29: the pointer of a pool the game keeps scrambled, as its
    // readers unscramble it (GET_STATE_OF_RAYFIRE_MAP_OBJECT 0x10C596D:
    // rol key 32, ror 3, xor, rol by (key & 31) + 1, rol 32, not). 0 while
    // the pool is not set up.
    inline std::uint64_t Rol64(std::uint64_t value, unsigned bits)
    {
        bits &= 63;
        return bits ? (value << bits) | (value >> (64 - bits)) : value;
    }

    inline std::uint64_t DecodePoolPointer(std::uint8_t ready, std::uint64_t scrambled, std::uint64_t key)
    {
        if (!ready)
            return 0;
        const std::uint64_t mixed = Rol64(Rol64(key, 32), 64 - 3);
        std::uint64_t value = scrambled ^ mixed;
        value = Rol64(value, (static_cast<unsigned>(mixed) & 0x1F) + 1);
        value = Rol64(value, 32);
        return ~value;
    }

    // A pool slot's flag byte: bit 7 set means free.
    inline bool PoolSlotLive(std::uint8_t flags) { return (flags & 0x80) == 0; }

    // Probe 25: handles other than -1 among a ped's cloth handles.
    inline int CountClothHandles(const std::int32_t* handles, std::size_t count)
    {
        int pieces = 0;
        for (std::size_t i = 0; handles && i < count; ++i)
            pieces += handles[i] != -1 ? 1 : 0;
        return pieces;
    }

    // Probe 19: SET_VEHICLE_LIGHTS keeps its state in the low 3 bits.
    inline int VehicleLightState(std::uint8_t field) { return field & 7; }

    inline double Length3(const float* xyz)
    {
        return xyz ? std::sqrt(static_cast<double>(xyz[0]) * xyz[0] + static_cast<double>(xyz[1]) * xyz[1] +
            static_cast<double>(xyz[2]) * xyz[2]) : 0.0;
    }

    // The game's string hash (lower case Jenkins one-at-a-time), for the
    // names the probe reads as text: the same value the hash ports carry.
    inline std::uint32_t HashLower(const char* text)
    {
        std::uint32_t hash = 0;
        for (const char* c = text; c && *c; ++c)
        {
            char lower = *c >= 'A' && *c <= 'Z' ? static_cast<char>(*c + 32) : *c;
            if (lower == '\\')
                lower = '/';
            hash += static_cast<std::uint8_t>(lower);
            hash += hash << 10;
            hash ^= hash >> 6;
        }
        hash += hash << 3;
        hash ^= hash >> 11;
        hash += hash << 15;
        return hash;
    }

    // Names read as text (a shake, a bank, a render target), kept once per
    // table and hash. The caller holds the probe's lock.
    struct NameRow
    {
        std::string table;
        std::uint32_t hash = 0;
        std::string text;
        std::uint64_t count = 0;
    };

    class NameTable
    {
    public:
        explicit NameTable(std::size_t capacity) : m_capacity(capacity) { m_rows.reserve(capacity); }

        // False when the table is full and the name is new.
        bool Add(const char* table, const char* text)
        {
            const std::string clean = Clean(text);
            const std::uint32_t hash = HashLower(clean.c_str());
            for (NameRow& row : m_rows)
            {
                if (row.hash == hash && row.table == table)
                {
                    ++row.count;
                    return true;
                }
            }
            if (m_rows.size() >= m_capacity)
            {
                ++m_overflow;
                return false;
            }
            NameRow row;
            row.table = table;
            row.hash = hash;
            row.text = clean;
            row.count = 1;
            m_rows.push_back(std::move(row));
            return true;
        }

        const std::vector<NameRow>& Rows() const { return m_rows; }
        std::uint64_t Overflow() const { return m_overflow; }

        // Printable, with no comma, equals sign or line break (the report's
        // separators), 63 characters at most.
        static std::string Clean(const char* text)
        {
            std::string out;
            for (const char* c = text; c && *c && out.size() < 63; ++c)
                out += *c > 32 && *c < 127 && *c != ',' && *c != '=' ? *c : '_';
            return out;
        }

    private:
        std::size_t m_capacity;
        std::vector<NameRow> m_rows;
        std::uint64_t m_overflow = 0;
    };

    // ------------------------------------------------------------ report

    struct PortRow
    {
        Port port = kShakeByName;
        PortReport data; // data.port is not used
    };

    struct SeriesRow
    {
        Series series = kFinalFov;
        std::string status; // ok, site_mismatch, not_seen
        SeriesStats stats;
    };

    struct KeyTableRows
    {
        std::string name;
        std::vector<KeyRow> rows;
        std::uint64_t overflow = 0;
    };

    struct SiteRow
    {
        std::string name;
        std::uint32_t rva = 0;
        bool ok = false;
    };

    struct ReportData
    {
        std::string version;
        std::string started;
        std::string written;
        bool final = false;
        double elapsedS = 0.0;
        double modeS[kModes] = {};
        std::uint64_t frames = 0;
        std::string build; // "1491.50" when every signature and site matched, else what differs
        std::vector<PortRow> ports;
        std::vector<soundfx::ThreadName> threads;
        std::vector<SeriesRow> series;
        std::vector<KeyTableRows> tables;
        std::vector<NameRow> names;
        std::uint64_t nameOverflow = 0;
        std::vector<SiteRow> sites;
        LightningTracker lightning;
        LightTracker lights;                   // probe 26: counts and the vanished samples
        std::uint64_t rayfireStateChanges = 0; // probe 29
        std::uint64_t pollNatives = 0;         // native calls made by the polls
        double pollMs = 0.0;                   // time spent in the polls
    };

    // "capture-probe-20261005-213012.txt"
    inline std::string ReportFileName(int year, int month, int day, int hour, int minute, int second)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "capture-probe-%04d%02d%02d-%02d%02d%02d.txt", year, month, day, hour, minute,
            second);
        return text;
    }

    // The whole report: one record per line, `TAG,key=value,...`, the first
    // line the header. Text in English, like every text of the mod.
    inline std::string FormatReport(const ReportData& data)
    {
        std::string out;
        out.reserve(1 << 16);
        out += "CAPTURE_PROBE_REPORT,format=1,version=" + data.version + ",started=" + data.started + ",written=" +
            data.written + ",final=" + (data.final ? "1" : "0") + ",elapsed_s=" + Number(data.elapsedS) +
            ",frames=" + std::to_string(data.frames);
        for (std::size_t mode = 0; mode < kModes; ++mode)
            out += std::string(",") + ModeName(mode) + "_s=" + Number(data.modeS[mode]);
        out += ",build=" + data.build + ",poll_natives=" + std::to_string(data.pollNatives) + ",poll_ms=" +
            Number(data.pollMs) + '\n';
        out += "# Read probes 14 to 30 of the capture panorama. Nothing here changed\n";
        out += "# the game: every detour forwards its call, every poll only reads. tools/capture_probe_report.py reads it.\n";
        for (const ProbeInfo& probe : kProbes)
            out += "PROBE,n=" + std::to_string(probe.number) + ",status=" + StatusName(probe.status) + ",name=" + probe.name +
                ",how=" + probe.how + ",note=" + probe.note + '\n';
        for (const SkippedInfo& skipped : kSkipped)
            out += "SKIPPED,probe=" + std::to_string(skipped.probe) + ",kind=" + skipped.kind + ",what=" + skipped.what +
                ",reason=" + skipped.reason + '\n';
        for (const SiteRow& site : data.sites)
            out += "SITE,name=" + site.name + ",rva=" + Hex(site.rva) + ",status=" + (site.ok ? "ok" : "mismatch") + '\n';

        for (const PortRow& row : data.ports)
        {
            const PortInfo& info = Info(row.port);
            const PortReport& port = row.data;
            const std::uint64_t total = port.Total();
            out += std::string("PORT,id=") + info.id + ",probe=" + std::to_string(info.probe) + ",rva=" + Hex(info.rva) +
                ",name=" + info.name + ",access=detour,status=" + port.status + ",calls=" + std::to_string(total);
            for (std::size_t mode = 0; mode < kModes; ++mode)
                out += std::string(",calls_") + ModeName(mode) + "=" + std::to_string(port.calls[mode]);
            out += ",per_s=" + Number(PerSecond(total, data.elapsedS)) +
                ",per_frame=" + Number(data.frames ? static_cast<double>(total) / static_cast<double>(data.frames) : 0.0) +
                ",max_frame=" + std::to_string(port.maxPerFrame) + ",threads=";
            for (std::size_t i = 0; i < port.threads.size(); ++i)
                out += (i ? ";" : "") + std::to_string(port.threads[i]);
            out += ",thread_overflow=" + std::to_string(port.threadOverflow) +
                ",callers=" + std::to_string(port.callers.size()) + ",caller_overflow=" + std::to_string(port.callerOverflow) +
                ",proof=" + info.proof + '\n';
            soundfx::detail::HistogramLine(out, info.id, 0, info.label0, port.values[0]);
            soundfx::detail::HistogramLine(out, info.id, 1, info.label1, port.values[1]);
            for (const soundfx::CallerRow& caller : port.callers)
                out += std::string("CALLER,id=") + info.id + ",rva=" + Hex(caller.rva) + ",calls=" +
                    std::to_string(caller.calls) + '\n';
            for (std::size_t i = 0; i < port.samples.size(); ++i)
            {
                const soundfx::Sample& sample = port.samples[i];
                out += std::string("SAMPLE,id=") + info.id + ",n=" + std::to_string(i) + ",caller=" + Hex(sample.caller) +
                    ",thread=" + std::to_string(sample.thread) + ",a0=" + Hex(sample.args[0]) + ",a1=" + Hex(sample.args[1]) +
                    ",a2=" + Hex(sample.args[2]) + ",a3=" + Hex(sample.args[3]) + ",value=" + Number(sample.value) + '\n';
            }
        }

        for (const soundfx::ThreadName& thread : data.threads)
            out += "THREAD,id=" + std::to_string(thread.id) + ",name=" + thread.name + '\n';

        for (const SeriesRow& row : data.series)
        {
            const SeriesInfo& info = Info(row.series);
            out += std::string("SERIES,probe=") + std::to_string(info.probe) + ",name=" + info.name + ",status=" +
                (row.stats.Samples() == 0 && row.status == "ok" ? "not_seen" : row.status) + ",samples=" +
                std::to_string(row.stats.Samples()) + ",unread=" + std::to_string(row.stats.unread) + ",source=" + info.source +
                '\n';
            for (std::size_t mode = 0; mode < kModes; ++mode)
            {
                const Histogram& h = row.stats.byMode[mode];
                if (!h.count)
                    continue;
                out += std::string("SERIES_MODE,name=") + info.name + ",mode=" + ModeName(mode) + ",count=" +
                    std::to_string(h.count) + ",min=" + Number(h.min) + ",max=" + Number(h.max) + ",mean=" + Number(h.Mean()) +
                    ",changes=" + std::to_string(row.stats.changes[mode]) + '\n';
            }
        }

        const LightningTracker& lightning = data.lightning;
        out += "LIGHTNING,started=" + std::to_string(lightning.Total()) + ",directional=" +
            std::to_string(lightning.started[1]) + ",cloud=" + std::to_string(lightning.started[2]) + ",strike=" +
            std::to_string(lightning.started[3]) + ",other_type=" + std::to_string(lightning.otherType) +
            ",request_rises=" + std::to_string(lightning.requestRises) + ",request_frames=" +
            std::to_string(lightning.requestFrames) + ",forced_frames=" + std::to_string(lightning.forcedFrames) + '\n';
        const LightTracker& lights = data.lights;
        out += "LIGHTS,went_dark=" + std::to_string(lights.wentDark) + ",came_on=" + std::to_string(lights.cameOn) +
            ",lit_vanished=" + std::to_string(lights.litVanished) + ",vanished_gone=" + std::to_string(lights.vanishedGone) +
            ",vanished_left=" + std::to_string(lights.vanishedLeft) + ",sweeps=" + std::to_string(lights.sweeps) +
            ",samples=" + std::to_string(lights.samples.size()) + '\n';
        for (std::size_t i = 0; i < lights.samples.size(); ++i)
        {
            const LitVanishedSample& sample = lights.samples[i];
            out += "LIGHT_VANISHED,n=" + std::to_string(i) + ",model=" + Hex(sample.model) + ",x=" + Number(sample.xyz[0]) +
                ",y=" + Number(sample.xyz[1]) + ",z=" + Number(sample.xyz[2]) + ",intensity=" + Number(sample.intensity) +
                ",exists=" + (sample.exists ? "1" : "0") + ",distance=" + Number(sample.distance) + ",new_model=" +
                Hex(sample.newModel) + ",new_distance=" + Number(sample.newDistance) + '\n';
        }
        out += "RAYFIRE,state_changes=" + std::to_string(data.rayfireStateChanges) + '\n';

        for (const KeyTableRows& table : data.tables)
        {
            out += "TABLE,name=" + table.name + ",rows=" + std::to_string(table.rows.size()) + ",overflow=" +
                std::to_string(table.overflow) + '\n';
            for (const KeyRow& row : table.rows)
            {
                out += "KEY,table=" + table.name + ",key=" + Hex(row.key) + ",count=" + std::to_string(row.count) +
                    ",flagged=" + std::to_string(row.flagged) + ",mask=" + std::to_string(row.mask);
                if (row.hasValue)
                    out += ",min=" + Number(row.min) + ",max=" + Number(row.max);
                out += '\n';
            }
        }
        out += "NAMES,rows=" + std::to_string(data.names.size()) + ",overflow=" + std::to_string(data.nameOverflow) + '\n';
        for (const NameRow& name : data.names)
            out += "NAME,table=" + name.table + ",hash=" + Hex(name.hash) + ",count=" + std::to_string(name.count) +
                ",text=" + name.text + '\n';
        out += "END\n";
        return out;
    }
}
