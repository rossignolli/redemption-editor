#pragma once

// The volume probe, pure part. A measurement, not a fix: it reads, in the
// recording and in the replay, the volume the audio engine gives every voice
// that plays (every audEnvironmentSound, the leaf sound that owns a voice),
// split into the stages the engine multiplies, grouped by what the sound is
// (script speech, ambient speech, steps, hooves, wind, weather, ambience,
// emitters, music...) per window of clip time. A diff of the two passes says
// who loses or gains volume and at which stage. Nothing here changes what the
// game plays.
//
// The walk reads the bucket's VOICE LIST (the list the audio frame
// processes), not the parent bitmap, so every playing voice is seen, not
// only the top-level environment sounds (the speech lines); the
// voices of the weather, ambience, emitter, particle, script and music
// entities get classes of their own; the residual of the chain is split into
// the mix group term and the mixer scene term the engine applies (what is
// left is occlusion + distance attenuation); quad speaker levels (the
// unpositioned beds: wind) are read; and the controls suspected of the loud
// wind are logged per window (VOLUME_PROBE_ENV).
//
// Engine layout (RDR2 1491.50; every site below is checked byte for byte at
// runtime before the first read, kSites):
//
// Sound pool (the pool the requested-settings processor 0x2533EF0 and the
// audio frame 0x2539D5C walk):
//   [0x53588D8] pool table; bucket b at table + b * 0x2A860 (0x2539F91,
//   0x258141F), buckets
//   [0x53588B8] (0x253A0DF cmp), slots per bucket [0x53588C0] (0x2539FC6),
//   sound slot = [bucket+0x2A848] + slot * [0x53588B0] (0x25326EB, 0x2539FB3),
//   requested settings block = [bucket+0x2A850] + index * [0x53588B4]
//   (0x2533FB9, 0x2533F9C).
//   bucket+0x2A440: one PARENT bit per slot (set only by the factory
//   0x252AAE0 at 0x252AC1B for a sound without a parent; 0x25326A0, which
//   reads it at 0x2532768, is the parent iterator). It holds only top-level
//   sounds.
//   bucket+0x2A846 u16: entries of the bucket's voice list (zeroed by the
//   frame at 0x2539FAA, bumped by 0x2581400 at 0x2581443, at most 0x200
//   0x258141A); bucket+0x29840 + i*2 u16: the environment sound's slot of
//   entry i (0x258149D, 0xFFFF a removed entry, read by the frame's iterator
//   0x25325C4 at 0x25325D4 / 0x25325ED / 0x2532602); bucket+0x24040 + i*0x2C:
//   the combine buffer of entry i (copied at 0x258145C; the frame passes it
//   to 0x256C3EC at 0x253A0A0/0x253A0A4): +0 the combined volume of the
//   hierarchy in dB, +4 its post-submix dB (0x256C730, 0x256C738).
// audSound (any type): +0xA0 bucket byte (0x253270C), +0xE2 parent slot
//   (0xFFFF none, 0x25326FC), +0xD6 requested settings index of a top-level
//   sound (0x2533F87), +0x48 tracker and +0x50 audio entity of a top-level
//   sound (0x2585DEF, 0x2585DA3).
// audEnvironmentSound (vtable 0x360E208; its update 0x2567D18 at slot +0x50
//   registers it in the voice list):
//   +0x240 u16 its voice index (0x2567D88; 0xFFFF once freed, 0x2573810),
//          +0x242 u16 its voice list entry: written by the registration
//          (0x2567FF9) and set back to 0xFFFF by the frame's metric builder
//          right after the voice is mixed (0x256D831 / 0x256D896, rdi = E,
//          r15d = 0xFFFF at 0x256D825; skipped only with r8b = 0, a lone
//          query 0x253A18C; the frame passes r8b = 1 at 0x253A09D).
//          Between two frames every mixed voice holds 0xFFFF there, so the
//          probe keeps an entry when E+0x242 is i (registered, not mixed yet)
//          or 0xFFFF (mixed), and refuses another index (backlink_bad);
//   +0xD8 u16 the requested settings index of its top sound, read straight
//          from the leaf (0x2568075; the metric builder 0x256C480 uses it);
//   +0x224 f32 the voice's final linear gain of this audio frame (0x256D644):
//          volume * (dry + wet) * chain;
//   +0x238 s16 category index (0x256CA3D) into the resolved category table
//          [0x5338488] (0x256CA51), entries of 0x48 bytes, +0 the category
//          volume in dB (0x256CC65);
//   +0x249 & 0x1F the voice's bus: with 0x25313D0(bus) true (0x11..0x20,
//          0x22, 0x23) the post-submix, the category and the mix groups are
//          NOT applied (0x256C747 zeroes the post, 0x256CC5B, 0x256CD60,
//          0x256CEBE skip);
//   +0x1A0 6 current mix group bytes, +0x1A6 the 6 previous ones (0xFF none,
//          0x256CCC9, 0x256CD79), each group slot j fading by the byte
//          S+0x64+6j / 255 (the metric copy of S+0x50 at rbp+0xB0, 0x256C889,
//          0x256C88D; 0x256CCB1, 0x256CE17): current group factor
//          1 + f*(v-1), previous group factor v + f*(1-v), v the resolved
//          volume of the group's patched category (group pool [0x5338558]
//          0x256CD93: +0x10 count, +0x38 used bits, record = [+8] + g*[+0x20],
//          record+8 the s16 category remap table, 0x256CDD0..0x256CDDF);
//   +0x134 u16 ids of the mixer scenes matched by the sound, count at +0x19C
//          (0x256CFB5, 0x256CFCB), filtered by the 6 s16 tags at +0x1AC
//          (0x256D03D): scene by id in the lists [0x53479D0]/[0x53479D8] and
//          [0x5347900]/[0x5347908] (0x252909C), +0x2008 id, +0x2000 tag (-1
//          matches all, 0x25316D0), +0x1FFC mode (0 multiply, 1 minimum,
//          0x256D168), +0 the linear volume (0x256D1A5) multiplied into the
//          chain (0x256D21A);
//   +0x108 the voice state V (0x256D600): V+0xD0 the combined volume made
//          linear after a -2 dB bias, V+0xD4 the chain: post-submix * category
//          * mix groups * scenes * occlusion * distance attenuation.
// Requested settings block S (double-buffered by the game index [0x533852C]
//   and the audio index [0x5338528], 0x256F4CB): requested volume dB
//   +0x120 + idx*0x40 (0x256F50B), post-submix dB +0x124 + idx*0x40
//   (0x256F514), dry +0x12C and wet +0x128 + idx*0x40 (0x256CAEC, 0x256CAFE),
//   position +0x10 + idx*0x10 (0x256C58F, 0x256C784); the position mode in
//   bits 2..3 of +0x25F (0x2536F7C/0x2536F83: 0 = no position); quad speaker
//   levels: SetRequestedQuadSpeakerLevels 0x2536ED4 writes 4 linear levels
//   into the position slot (0x2536EE8), clears the position mode
//   (0x2536EFE) and sets bit 0x8000 of +0x14C + idx*0x40 (0x2536F0F): an
//   unpositioned bed (the wind slots, 0x31C6D30); occlusion "open" S+0xD4,
//   environment group S+0x248.
//
// Global audio entities (static objects of the image; the probe checks the
// first qword against the vtable the constructor writes): weather
// 0x5C90070 (vtable 0x383D3B0, static init 0x112D90/0x112D9C), ambience
// 0x5CA7BB0 (0x383F010, 0x1154E8 + ctor 0x31FA632; its 0x3200D20 loads
// AMBIENT_ZONE_LIST), static emitters 0x5C505C0 (0x3832418, 0x10D8E4 + ctor
// 0x3100620; SET_STATIC_EMITTER_ENABLED), particle emitters 0x5C9A660
// (0x383ECF0, 0x11555A/0x115566), script audio 0x5C81C20 (0x383D4A8,
// 0x112D24 + ctor 0x31B997D; SET_AUDIO_FLAG, START_AUDIO_SCENE,
// SET_AMBIENT_ZONE_STATE), music manager [0x5C7D2B8] (heap, vtable
// 0x3837D48, ctor 0x316A4F7; AUDIO_IS_MUSIC_PLAYING 0xF5E3FD reads it; its
// two track slots at +0x6A8 + k*0x390 hold the sound at +8, 0x316FB78,
// 0x316FB7F, 0x316FB88).
// Weather entity W: the wind block at W+0x1980 (0x31B874B), its 12 sound
//   slots (2 x 6) at block+0xF90 + i*0x130 with the sound at +0 (0x31CF136,
//   0x31CF141, 0x31CF145, 0x31CF14B, 0x31CF1AE); wind strength smoothed
//   block+0x35C4 = W+0x4F44 (0x31CCC36); script wind type override W+0x50A8
//   with the hashes W+0x509C/W+0x50A0 (0x31B8846, 0x31B884F, 0x31B8864); the
//   wind variable 0x1C6CE2B3 override W+0x508C with its value W+0x5074
//   (0x31BDC88, 0x31BDC91); the weather mixer scene W+0x5068 (0x31CEF79);
//   weather types: indices [0x4576204] (previous) and [0x4576208] (next) in
//   the table 0x45722E0, stride 0x1E0, hash at +0x10 (0x31B8737..0x31B8759).
// Interior: 0x5B83300 (0x31CCC13), in interior byte +0x343D (0x31D73ED),
//   interior ratio +0x3444 (0x31D7403).
// Exposure per direction (4 floats): 0x5C97790+0xBF4+dir*4 (0x31C6E7E,
//   0x31E1D9F); multiplied into the wind slots' levels.
// Speech duck (the conversation self-duck the speech analysis found):
//   level 0x5B5BF24 (0x2FCC2D9), flag 0x5B82EDE (0x2F2D300), mixer scene
//   pointer 0x5BA5FE0 (0x2F2D255).
// Audio flags (SET_AUDIO_FLAG): 88 counters at 0x5C81C20+0xCE6C+4*i
//   (0x31C62BF, 0x31C6457; index 65 OpenWorldMusicDisabled read at
//   0x317391D), names hashed in the table 0x383CDE0..0x383CF40 (0x31C6243,
//   0x31C6251).
// The listener: the game's audio environment volume points
//   (game_listener_core.h, already proven).
// Speech: a line's data (line +0x1D8) holds its sounds at +0xC0 (0x2FA10C0)
//   and the layers at +0x198 + i*8 (0x2FA0DB9).
//
// Pure: unit tested in ReplayCoreTests (volume_probe_tests.cpp).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace replay::volumeprobe
{
    // ------------------------------------------------------------ layout

    constexpr std::uint32_t kEnvVtableRva = 0x360E208;
    constexpr std::uint32_t kEnvPlayRva = 0x2567D18;   // vtable slot +0x50
    constexpr std::uint32_t kSoundSlot1Rva = 0x25735EC; // vtable slot +0x08 of every sound (S38)
    constexpr std::uint32_t kPoolTableRva = 0x53588D8;
    constexpr std::uint32_t kSoundStrideRva = 0x53588B0;
    constexpr std::uint32_t kSettingsStrideRva = 0x53588B4;
    constexpr std::uint32_t kBucketCountRva = 0x53588B8;
    constexpr std::uint32_t kSlotCountRva = 0x53588C0;
    constexpr std::uint32_t kAudioIndexRva = 0x5338528;
    constexpr std::uint32_t kCategoryTableRva = 0x5338488;
    constexpr std::uint32_t kEnvSyncRva = 0x53587F4; // byte: 1 the environment result is recomputed every frame
    constexpr std::uint32_t kGroupPoolRva = 0x5338558;
    constexpr std::uint32_t kSceneListARva = 0x53479D0;
    constexpr std::uint32_t kSceneCountARva = 0x53479D8;
    constexpr std::uint32_t kSceneListBRva = 0x5347900;
    constexpr std::uint32_t kSceneCountBRva = 0x5347908;
    constexpr std::uint32_t kGameIndexRva = 0x533852C;
    constexpr std::uint32_t kFadeScaleRva = 0x33359A0; // .rdata f32 1/255 (0x256CCA9)

    constexpr std::uint32_t kBucketStride = 0x2A860;
    constexpr std::uint32_t kBucketParentBits = 0x2A440; // parent bits, not used bits
    constexpr std::uint32_t kBucketSounds = 0x2A848;
    constexpr std::uint32_t kBucketSettings = 0x2A850;
    constexpr std::uint32_t kBucketVoiceCount = 0x2A846;
    constexpr std::uint32_t kBucketVoiceSlots = 0x29840;
    constexpr std::uint32_t kBucketCombine = 0x24040;
    constexpr std::uint32_t kCombineStride = 0x2C;
    constexpr std::uint32_t kMaxVoicesPerBucket = 0x200;

    constexpr std::uint32_t kSoundBucket = 0xA0;
    constexpr std::uint32_t kSoundParent = 0xE2;
    constexpr std::uint32_t kSoundSettingsIndex = 0xD6;
    constexpr std::uint32_t kSoundTracker = 0x48;
    constexpr std::uint32_t kSoundEntity = 0x50;
    constexpr std::uint32_t kSoundChildren = 0xB6; // u16[16] (documented, not walked)
    // Play state in the two top bits of +0xF2 (audio_observer.h SoundPlayState;
    // 0x316FB9A tests (+0xF2 & 0xC0) == 0x80 for a playing track): 2 = playing.
    constexpr std::uint32_t kSoundState = 0xF2;
    constexpr std::uint8_t kStatePlaying = 2;
    constexpr std::uint16_t kNoIndex = 0xFFFF;

    constexpr std::uint32_t kEnvFinal = 0x224;
    constexpr std::uint32_t kEnvCategory = 0x238;
    constexpr std::uint32_t kEnvMixGroups = 0x1A0;
    constexpr std::uint32_t kEnvMixGroupCount = 12; // 6 current + 6 previous
    constexpr std::uint32_t kGroupSlots = 6;
    constexpr std::uint32_t kEnvPrevGroups = 0x1A6;
    constexpr std::uint32_t kEnvTags = 0x1AC;       // 6 s16
    constexpr std::uint32_t kEnvSceneIds = 0x134;   // u16
    constexpr std::uint32_t kEnvSceneCount = 0x19C; // s32
    constexpr std::uint32_t kMaxSceneIds = (kEnvSceneCount - kEnvSceneIds) / 2; // 52
    constexpr std::uint32_t kEnvVoiceIndex = 0x240;
    constexpr std::uint32_t kEnvListIndex = 0x242;
    constexpr std::uint32_t kEnvBus = 0x249;
    constexpr std::uint32_t kEnvTopSettings = 0xD8;
    constexpr std::uint32_t kEnvVoice = 0x108;
    constexpr std::uint32_t kEnvBytes = 0x250; // the type's size (the size table at 0x253CDC8)

    constexpr std::uint32_t kVoiceVolume = 0xD0;
    constexpr std::uint32_t kVoiceChain = 0xD4;

    constexpr std::uint32_t kCategoryStride = 0x48;
    constexpr std::uint32_t kCategoryVolume = 0x0;
    constexpr std::int32_t kMaxCategory = 4096; // sanity bound
    constexpr float kCatFloorDb = -99.0f;      // 0x360C7F4: below it the engine uses 0 (silence)

    constexpr std::uint32_t kGroupPoolBase = 0x08;
    constexpr std::uint32_t kGroupPoolCount = 0x10;
    constexpr std::uint32_t kGroupPoolStride = 0x20;
    constexpr std::uint32_t kGroupPoolBits = 0x38;
    constexpr std::uint32_t kGroupRemap = 0x08;
    constexpr std::uint32_t kMaxGroups = 256;

    // The mixer's scene instances (the lists of 0x252909C; constructor
    // 0x2519C4C: +0 volume 1.0, +0x1FFC mode 0, +0x2000 tag -1, +0x2004 its
    // argument, +0x2008 id).
    constexpr std::uint32_t kSceneVolume = 0x0;
    constexpr std::uint32_t kSceneMode = 0x1FFC;
    constexpr std::uint32_t kSceneTag = 0x2000;
    constexpr std::uint32_t kSceneKey = 0x2004;
    constexpr std::uint32_t kSceneId = 0x2008;
    constexpr std::uint32_t kMaxScenesPerList = 512;
    // The game's scene objects (0x340 bytes from the mixer's pool,
    // 0x2F18E40, the ones a scene reference holds: W+0x5068, 0x5BA5FE0):
    // the hash at +0x328 (0x2F19087), the variables at +0x18 (pairs of name
    // hash and float, count at +0x68 up to 10, 0x3279628).
    constexpr std::uint32_t kGameSceneHash = 0x328;
    constexpr std::uint32_t kGameSceneVars = 0x18;
    constexpr std::uint32_t kGameSceneVarCount = 0x68;
    constexpr std::uint32_t kGameSceneMaxVars = 10;
    constexpr std::uint32_t kApplyHash = 0xE865CDE8; // joaat("apply")
    // Two of the variables the speech manager writes into the speech duck scene
    // ("speech_scene", 0x576520E1, started by 0x2F2D1EC through 0x2F18E40,
    // held at 0x5BA5FE0): what it applies to the weather and to the sfx.
    constexpr std::uint32_t kWeatherApplyHash = 0x965062BE; // joaat("weatherApply")
    constexpr std::uint32_t kSfxApplyHash = 0x934A8049;     // joaat("sfxApply")

    constexpr std::uint32_t kSettingsPosition = 0x10;  // + audio index * 0x10 (quad levels when bit 0x8000)
    constexpr std::uint32_t kSettingsMetric = 0x50;    // copied to the metric (rbp+0xB0)
    constexpr std::uint32_t kSettingsGroupIds = 0x60;  // + 6j: s16 current, s16 previous, u8 fade
    constexpr std::uint32_t kSettingsOpen = 0xD4;
    constexpr std::uint32_t kSettingsVolume = 0x120;   // + audio index * 0x40
    constexpr std::uint32_t kSettingsPost = 0x124;
    constexpr std::uint32_t kSettingsWet = 0x128;
    constexpr std::uint32_t kSettingsDry = 0x12C;
    constexpr std::uint32_t kSettingsSpeakerFlags = 0x14C; // u16 + audio index * 0x40, 0x8000 quad levels
    constexpr std::uint16_t kQuadFlag = 0x8000;
    constexpr std::uint32_t kSettingsBufferStride = 0x40;
    constexpr std::uint32_t kSettingsEnvGroup = 0x248;
    constexpr std::uint32_t kSettingsPosMode = 0x25F; // bits 2..3
    constexpr std::uint32_t kSettingsMinBytes = 0x262; // 0x256C4A9 reads +0x261
    constexpr std::uint32_t kSettingsBytes = 0x270;    // copied: min(stride, this)

    constexpr std::uint32_t kLineDataSound = 0xC0;
    constexpr std::uint32_t kLineDataLayers = 0x198;
    constexpr std::uint32_t kLineDataLayerCount = 5;

    constexpr float kVolumeBiasDb = 2.0f; // 0x256C7E7 / 0x2525758
    constexpr float kFadeScale = 1.0f / 255.0f;

    // Global entities and the controls (RVAs of .data).
    constexpr std::uint32_t kWeatherRva = 0x5C90070;
    constexpr std::uint32_t kWeatherVtableRva = 0x383D3B0;
    constexpr std::uint32_t kAmbientRva = 0x5CA7BB0;
    constexpr std::uint32_t kAmbientVtableRva = 0x383F010;
    constexpr std::uint32_t kEmitterRva = 0x5C505C0;
    constexpr std::uint32_t kEmitterVtableRva = 0x3832418;
    constexpr std::uint32_t kPtfxRva = 0x5C9A660;
    constexpr std::uint32_t kPtfxVtableRva = 0x383ECF0;
    constexpr std::uint32_t kScriptAudioRva = 0x5C81C20;
    constexpr std::uint32_t kScriptAudioVtableRva = 0x383D4A8;
    constexpr std::uint32_t kMusicPtrRva = 0x5C7D2B8;
    constexpr std::uint32_t kMusicVtableRva = 0x3837D48;
    constexpr std::uint32_t kMusicTracks = 0x6A8;
    constexpr std::uint32_t kMusicTrackStride = 0x390;
    constexpr std::uint32_t kMusicTrackSound = 0x08;
    constexpr std::uint32_t kMusicTrackCount = 2;

    constexpr std::uint32_t kWindBlock = 0x1980;
    constexpr std::uint32_t kWindSlots = 0xF90;
    constexpr std::uint32_t kWindSlotStride = 0x130;
    constexpr std::uint32_t kWindSlotCount = 12;
    constexpr std::uint32_t kWindStrength = 0x35C4; // in the block: W+0x4F44
    constexpr std::uint32_t kWindTypeOverride = 0x50A8;
    constexpr std::uint32_t kWindTypeOld = 0x509C;
    constexpr std::uint32_t kWindTypeNew = 0x50A0;
    constexpr std::uint32_t kWindVarOverride = 0x508C;
    constexpr std::uint32_t kWindVarValue = 0x5074;
    constexpr std::uint32_t kWeatherScene = 0x5068;
    constexpr std::uint32_t kWeatherPrevRva = 0x4576204;
    constexpr std::uint32_t kWeatherNextRva = 0x4576208;
    constexpr std::uint32_t kWeatherTableRva = 0x45722E0;
    constexpr std::uint32_t kWeatherTypeStride = 0x1E0;
    constexpr std::uint32_t kWeatherTypeHash = 0x10;
    constexpr std::uint32_t kMaxWeatherTypes = 256;

    constexpr std::uint32_t kInteriorRva = 0x5B83300;
    constexpr std::uint32_t kInteriorIn = 0x343D;
    constexpr std::uint32_t kInteriorRatio = 0x3444;
    constexpr std::uint32_t kExposureRva = 0x5C97790;
    constexpr std::uint32_t kExposure = 0xBF4;

    constexpr std::uint32_t kDuckLevelRva = 0x5B5BF24;
    constexpr std::uint32_t kDuckFlagRva = 0x5B82EDE;
    constexpr std::uint32_t kDuckSceneRva = 0x5BA5FE0;

    constexpr std::uint32_t kAudioFlagCountersRva = 0x5C8EA8C; // 0x5C81C20 + 0xCE6C
    constexpr std::uint32_t kAudioFlagTableRva = 0x383CDE0;
    constexpr std::uint32_t kAudioFlagCount = 88;
    static_assert(kAudioFlagCountersRva == kScriptAudioRva + 0xCE6C, "flag counters");
    static_assert(kAudioFlagCountersRva + 65 * 4 == 0x5C8EB90, "OpenWorldMusicDisabled counter (0x317391D)");

    // ------------------------------------------------------------ sites

    // Code sites checked once before any read (bytes of build 1491.50).
    // Part: 0 = the walk (a mismatch disables the probe); the others only
    // turn their own reads off (VOLUME_PROBE_CHECK,ok=0,part=...).
    enum class Part : std::uint8_t
    {
        Walk = 0,  // pool, voice list, voice stages, settings, speech
        Split,     // mix group and mixer scene terms of the chain
        Speaker,   // quad levels and position mode
        Entities,  // global audio entities, wind slots, music tracks
        Env,       // weather controls, interior, exposure, speech duck, audio flags
        Wrapper,   // the top wrapper sound's volume variable (wvol)
    };
    constexpr std::uint8_t kPartCount = 6;

    inline const char* PartName(Part part)
    {
        static const char* const kNames[kPartCount] = {"walk", "split", "speaker", "entities", "env", "wrapper"};
        return static_cast<std::uint8_t>(part) < kPartCount ? kNames[static_cast<std::uint8_t>(part)] : "?";
    }

    struct Site
    {
        const char* name;
        std::uint32_t rva;
        std::uint8_t size;
        std::uint8_t bytes[12];
        Part part;
    };
    constexpr Site kSites[] = {
        // The walk's stages, settings, speech
        {"final_store", 0x256D644, 8, {0xF3, 0x0F, 0x11, 0x8F, 0x24, 0x02, 0x00, 0x00}, Part::Walk},
        {"cat_index", 0x256CA3D, 8, {0x48, 0x0F, 0xBF, 0x87, 0x38, 0x02, 0x00, 0x00}, Part::Walk},
        {"cat_table", 0x256CA51, 7, {0x48, 0x8B, 0x05, 0x30, 0xBA, 0xDC, 0x02}, Part::Walk},
        {"cat_volume", 0x256CC65, 5, {0xF3, 0x0F, 0x10, 0x0C, 0xCB}, Part::Walk},
        {"voice_volume", 0x253645A, 8, {0xF3, 0x0F, 0x11, 0x89, 0xD0, 0x00, 0x00, 0x00}, Part::Walk},
        {"voice_chain", 0x253646A, 8, {0xF3, 0x0F, 0x11, 0x81, 0xD4, 0x00, 0x00, 0x00}, Part::Walk},
        {"voice_ptr", 0x256D600, 7, {0x4C, 0x8B, 0x8F, 0x08, 0x01, 0x00, 0x00}, Part::Walk},
        {"mix_groups", 0x256CCC9, 7, {0x4C, 0x8D, 0xBF, 0xA0, 0x01, 0x00, 0x00}, Part::Walk},
        {"sound_stride", 0x2539FB3, 6, {0x8B, 0x05, 0xF7, 0xE8, 0xE1, 0x02}, Part::Walk},
        {"pool_table", 0x2539FB9, 7, {0x48, 0x8B, 0x0D, 0x18, 0xE9, 0xE1, 0x02}, Part::Walk},
        {"slot_count", 0x2539FC6, 6, {0x8B, 0x05, 0xF4, 0xE8, 0xE1, 0x02}, Part::Walk},
        {"bucket_count", 0x253A0DF, 7, {0x44, 0x3B, 0x35, 0xD2, 0xE7, 0xE1, 0x02}, Part::Walk},
        {"parent_bits", 0x2539FD2, 6, {0x8B, 0x81, 0x40, 0xA4, 0x02, 0x00}, Part::Walk},
        {"sounds", 0x25326EB, 7, {0x4C, 0x03, 0x98, 0x48, 0xA8, 0x02, 0x00}, Part::Walk},
        {"settings_base", 0x2533FB9, 8, {0x4E, 0x03, 0x8C, 0x01, 0x50, 0xA8, 0x02, 0x00}, Part::Walk},
        {"settings_stride", 0x2533F9C, 6, {0x8B, 0x15, 0x12, 0x49, 0xE2, 0x02}, Part::Walk},
        {"audio_index", 0x256F4CB, 6, {0x8B, 0x0D, 0x57, 0x90, 0xDC, 0x02}, Part::Walk},
        {"req_volume", 0x256F50B, 9, {0xF3, 0x0F, 0x10, 0x84, 0x11, 0x20, 0x01, 0x00, 0x00}, Part::Walk},
        {"req_post", 0x256F514, 9, {0xF3, 0x0F, 0x10, 0x8C, 0x11, 0x24, 0x01, 0x00, 0x00}, Part::Walk},
        {"dry", 0x256CAEC, 10, {0xF3, 0x43, 0x0F, 0x10, 0x8C, 0x37, 0x2C, 0x01, 0x00, 0x00}, Part::Walk},
        {"wet", 0x256CAFE, 10, {0xF3, 0x43, 0x0F, 0x10, 0x84, 0x37, 0x28, 0x01, 0x00, 0x00}, Part::Walk},
        {"position", 0x256C784, 5, {0xF3, 0x0F, 0x6F, 0x4B, 0x10}, Part::Walk},
        {"position_index", 0x256C58F, 7, {0x44, 0x8B, 0x3D, 0x92, 0xBF, 0xDC, 0x02}, Part::Walk},
        {"metric_copy", 0x256C889, 4, {0x49, 0x8D, 0x56, 0x50}, Part::Walk},
        {"occlusion_open", 0x251F7BF, 9, {0xF3, 0x41, 0x0F, 0x10, 0x87, 0xF4, 0x00, 0x00, 0x00}, Part::Walk},
        // The store of the setter, not its first instruction (the entry
        // 0x2536948 is the S30 port: SoundFxProbe=1 hooks it).
        {"env_group", 0x253696F, 7, {0x48, 0x89, 0x91, 0x48, 0x02, 0x00, 0x00}, Part::Walk},
        {"parent", 0x25326FC, 8, {0x41, 0x0F, 0xB7, 0xBB, 0xE2, 0x00, 0x00, 0x00}, Part::Walk},
        {"bucket", 0x253270C, 8, {0x41, 0x0F, 0xB6, 0x83, 0xA0, 0x00, 0x00, 0x00}, Part::Walk},
        {"settings_index", 0x2533F87, 8, {0x44, 0x0F, 0xB7, 0x8B, 0xD6, 0x00, 0x00, 0x00}, Part::Walk},
        {"entity", 0x2585DA3, 4, {0x48, 0x8B, 0x4B, 0x50}, Part::Walk},
        {"tracker", 0x2585DEF, 4, {0x48, 0x8B, 0x4B, 0x48}, Part::Walk},
        {"speech_sound", 0x2FA10C0, 7, {0x48, 0x8D, 0x9F, 0xC0, 0x00, 0x00, 0x00}, Part::Walk},
        {"speech_layers", 0x2FA0DB9, 8, {0x48, 0x8D, 0x3C, 0xC5, 0x98, 0x01, 0x00, 0x00}, Part::Walk},
        {"env_sync", 0x256D3AC, 7, {0x40, 0x38, 0x35, 0x41, 0xB4, 0xDE, 0x02}, Part::Walk},
        {"total_store", 0x2525D93, 9, {0xF3, 0x41, 0x0F, 0x11, 0xB7, 0xF0, 0x00, 0x00, 0x00}, Part::Walk},
        // The voice list
        {"list_count_zero", 0x2539FAA, 9, {0x66, 0x41, 0x89, 0x94, 0x04, 0x46, 0xA8, 0x02, 0x00}, Part::Walk},
        {"list_slot_read", 0x25325D4, 9, {0x41, 0x0F, 0xB7, 0x94, 0x41, 0x40, 0x98, 0x02, 0x00}, Part::Walk},
        {"list_count_read", 0x25325ED, 8, {0x41, 0x0F, 0xB7, 0x81, 0x46, 0xA8, 0x02, 0x00}, Part::Walk},
        {"list_sound", 0x2532602, 7, {0x49, 0x03, 0x81, 0x48, 0xA8, 0x02, 0x00}, Part::Walk},
        {"list_count_bump", 0x2581443, 9, {0x66, 0x42, 0x89, 0x84, 0x09, 0x46, 0xA8, 0x02, 0x00}, Part::Walk},
        {"list_combine", 0x258145C, 9, {0x42, 0x0F, 0x11, 0x84, 0x0A, 0x40, 0x40, 0x02, 0x00}, Part::Walk},
        {"list_slot_store", 0x258149D, 8, {0x66, 0x89, 0xB4, 0x51, 0x40, 0x98, 0x02, 0x00}, Part::Walk},
        {"list_cap", 0x258141A, 5, {0xB8, 0x00, 0x02, 0x00, 0x00}, Part::Walk},
        {"env_list_index", 0x2567FF9, 7, {0x66, 0x89, 0x86, 0x42, 0x02, 0x00, 0x00}, Part::Walk},
        {"env_voice_index", 0x2567D88, 7, {0x66, 0x89, 0x86, 0x40, 0x02, 0x00, 0x00}, Part::Walk},
        {"combine_stride", 0x253A0A0, 4, {0x48, 0x6B, 0xD0, 0x2C}, Part::Walk},
        {"combine_frame", 0x253A0A4, 7, {0x48, 0x81, 0xC2, 0x40, 0x40, 0x02, 0x00}, Part::Walk},
        {"combine_volume", 0x256C730, 5, {0xF3, 0x41, 0x0F, 0x10, 0x31}, Part::Walk},
        {"combine_post", 0x256C738, 6, {0xF3, 0x45, 0x0F, 0x10, 0x41, 0x04}, Part::Walk},
        {"top_settings", 0x2568075, 7, {0x0F, 0xB7, 0x96, 0xD8, 0x00, 0x00, 0x00}, Part::Walk},
        {"parent_iter", 0x2532768, 7, {0x8B, 0x8C, 0x88, 0x40, 0xA4, 0x02, 0x00}, Part::Walk},
        // The frame's metric builder sets E+0x242
        // back to 0xFFFF after mixing the voice (the walk's Link), only when
        // called with r8b = 1 (the frame's call), and FreeVoice clears
        // E+0x240 (the check that keeps a freed voice out).
        {"metric_list_reset_value", 0x256D825, 6, {0x41, 0xBF, 0xFF, 0xFF, 0x00, 0x00}, Part::Walk},
        {"metric_list_reset", 0x256D831, 8, {0x66, 0x44, 0x89, 0xBF, 0x42, 0x02, 0x00, 0x00}, Part::Walk},
        {"metric_list_reset2", 0x256D896, 8, {0x66, 0x44, 0x89, 0xBF, 0x42, 0x02, 0x00, 0x00}, Part::Walk},
        {"metric_skip_flag", 0x256D2C2, 7, {0x40, 0x38, 0xB5, 0x60, 0x06, 0x00, 0x00}, Part::Walk},
        {"metric_this", 0x256C41E, 3, {0x48, 0x8B, 0xF9}, Part::Walk},
        {"frame_metric_flag", 0x253A09D, 3, {0x41, 0xB0, 0x01}, Part::Walk},
        {"voice_free_index", 0x2573810, 7, {0x66, 0x89, 0xBB, 0x40, 0x02, 0x00, 0x00}, Part::Walk},
        // The bucket stride, the one walk constant no other site covers: imul r12,rbx,0x2A860 in the audio frame (bucket b) and
        // imul r9,rdi,0x2A860 in the voice list insert 0x2581400.
        {"bucket_stride", 0x2539F91, 7, {0x4C, 0x69, 0xE3, 0x60, 0xA8, 0x02, 0x00}, Part::Walk},
        {"list_bucket_stride", 0x258141F, 7, {0x4C, 0x69, 0xCF, 0x60, 0xA8, 0x02, 0x00}, Part::Walk},
        // The chain's split (bus, mix groups, mixer scenes)
        {"bus_byte", 0x256CC3B, 7, {0x0F, 0xB6, 0x8F, 0x49, 0x02, 0x00, 0x00}, Part::Split},
        {"bus_check", 0x25313D0, 12, {0x8D, 0x41, 0xEF, 0x83, 0xF8, 0x0F, 0x76, 0x0B, 0x8D, 0x41, 0xDE, 0x83}, Part::Split},
        {"bus_check2", 0x25313DC, 10, {0xF8, 0x01, 0x76, 0x03, 0x32, 0xC0, 0xC3, 0xB0, 0x01, 0xC3}, Part::Split},
        {"group_pool", 0x256CD93, 7, {0x48, 0x8B, 0x1D, 0xBE, 0xB7, 0xDC, 0x02}, Part::Split},
        {"group_count", 0x256CD9F, 4, {0x44, 0x3B, 0x4B, 0x10}, Part::Split},
        {"group_bits", 0x256CDA5, 4, {0x48, 0x8B, 0x53, 0x38}, Part::Split},
        {"group_stride", 0x256CDBF, 4, {0x48, 0x8B, 0x53, 0x20}, Part::Split},
        {"group_base", 0x256CDC7, 4, {0x48, 0x03, 0x53, 0x08}, Part::Split},
        {"group_remap", 0x256CDD0, 4, {0x48, 0x8B, 0x42, 0x08}, Part::Split},
        {"group_remap_entry", 0x256CDDF, 4, {0x66, 0x39, 0x14, 0x48}, Part::Split},
        {"group_cat_table", 0x256CDEE, 7, {0x48, 0x8B, 0x05, 0x93, 0xB6, 0xDC, 0x02}, Part::Split},
        {"group_prev", 0x256CD79, 4, {0x4D, 0x8D, 0x47, 0x06}, Part::Split},
        {"group_cur_skip", 0x256CD40, 9, {0x43, 0x80, 0xBC, 0x3E, 0x60, 0xFE, 0xFF, 0xFF, 0xFF}, Part::Split},
        {"group_fade_ptr", 0x256CCB1, 7, {0x4C, 0x8D, 0xAD, 0xC4, 0x00, 0x00, 0x00}, Part::Split},
        {"group_fade", 0x256CE17, 5, {0x41, 0x0F, 0xB6, 0x45, 0x00}, Part::Split},
        {"group_fade_scale", 0x256CCA9, 8, {0xF3, 0x0F, 0x10, 0x35, 0xEF, 0x8C, 0xDC, 0x00}, Part::Split},
        {"metric_dest", 0x256C88D, 7, {0x48, 0x8D, 0x8D, 0xB0, 0x00, 0x00, 0x00}, Part::Split},
        {"cat_floor", 0x256C7DE, 9, {0xF3, 0x44, 0x0F, 0x10, 0x0D, 0x0D, 0x00, 0x0A, 0x01}, Part::Split},
        {"scene_count", 0x256CFB5, 6, {0x8B, 0x9F, 0x9C, 0x01, 0x00, 0x00}, Part::Split},
        {"scene_ids", 0x256CFCB, 8, {0x0F, 0xB7, 0x8C, 0x5F, 0x34, 0x01, 0x00, 0x00}, Part::Split},
        {"scene_tags", 0x256D03D, 7, {0x48, 0x8D, 0x97, 0xAC, 0x01, 0x00, 0x00}, Part::Split},
        {"scene_list_a", 0x252909C, 7, {0x48, 0x8B, 0x15, 0x2D, 0xE9, 0xE1, 0x02}, Part::Split},
        {"scene_count_a", 0x25290A3, 7, {0x0F, 0xB7, 0x05, 0x2E, 0xE9, 0xE1, 0x02}, Part::Split},
        {"scene_id", 0x25290B3, 7, {0x66, 0x3B, 0x88, 0x08, 0x20, 0x00, 0x00}, Part::Split},
        {"scene_list_b", 0x25290C5, 7, {0x48, 0x8B, 0x15, 0x34, 0xE8, 0xE1, 0x02}, Part::Split},
        {"scene_count_b", 0x25290CC, 7, {0x0F, 0xB7, 0x05, 0x35, 0xE8, 0xE1, 0x02}, Part::Split},
        {"scene_tag", 0x25316D0, 7, {0x44, 0x8B, 0x89, 0x00, 0x20, 0x00, 0x00}, Part::Split},
        {"scene_mode", 0x256D168, 6, {0x8B, 0x8B, 0xFC, 0x1F, 0x00, 0x00}, Part::Split},
        {"scene_volume", 0x256D1A5, 4, {0xF3, 0x0F, 0x59, 0x33}, Part::Split},
        {"scene_apply", 0x256D21A, 4, {0xF3, 0x0F, 0x59, 0xC6}, Part::Split},
        // Speaker levels and position mode.
        // The second read of the game index in the same function (0x2536ED4
        // is the function's entry, the S17 port that SoundGeneric=1 hooks).
        {"quad_index", 0x2536EEC, 6, {0x8B, 0x05, 0x3A, 0x16, 0xE0, 0x02}, Part::Speaker},
        {"quad_store", 0x2536EE8, 4, {0x0F, 0x11, 0x04, 0xC1}, Part::Speaker},
        {"quad_posmode", 0x2536EFE, 7, {0x80, 0xA1, 0x5F, 0x02, 0x00, 0x00, 0xF3}, Part::Speaker},
        {"quad_flag", 0x2536F0F, 8, {0x66, 0x09, 0x94, 0x08, 0x4C, 0x01, 0x00, 0x00}, Part::Speaker},
        // The or of the setter (0x2536F7C is its entry, the S35 port).
        {"posmode_set", 0x2536F86, 6, {0x08, 0x91, 0x5F, 0x02, 0x00, 0x00}, Part::Speaker},
        {"posmode_shift", 0x2536F83, 3, {0xC0, 0xE2, 0x02}, Part::Speaker},
        // Global audio entities, wind slots, music tracks
        {"weather_init", 0x112D90, 7, {0x48, 0x8D, 0x0D, 0xD9, 0xD2, 0xB7, 0x05}, Part::Entities},
        {"weather_vtable", 0x112D9C, 7, {0x48, 0x8D, 0x05, 0x0D, 0xA6, 0x72, 0x03}, Part::Entities},
        {"weather_wind", 0x31CF175, 7, {0x48, 0x8D, 0x0D, 0xF4, 0x0E, 0xAC, 0x02}, Part::Entities},
        {"wind_block", 0x31B874B, 7, {0x4C, 0x8D, 0xAE, 0x80, 0x19, 0x00, 0x00}, Part::Entities},
        {"wind_slots", 0x31CF136, 7, {0x4C, 0x8D, 0xB7, 0x90, 0x0F, 0x00, 0x00}, Part::Entities},
        {"wind_rows", 0x31CF141, 4, {0x44, 0x8D, 0x68, 0x02}, Part::Entities},
        {"wind_cols", 0x31CF145, 6, {0x41, 0xBC, 0x06, 0x00, 0x00, 0x00}, Part::Entities},
        {"wind_sound", 0x31CF14B, 3, {0x49, 0x8B, 0x1E}, Part::Entities},
        {"wind_stride", 0x31CF1AE, 7, {0x49, 0x81, 0xC6, 0x30, 0x01, 0x00, 0x00}, Part::Entities},
        {"ambient_init", 0x1154E8, 7, {0x48, 0x8D, 0x0D, 0xC1, 0x26, 0xB9, 0x05}, Part::Entities},
        {"ambient_vtable", 0x31FA632, 7, {0x48, 0x8D, 0x05, 0xD7, 0x49, 0x64, 0x00}, Part::Entities},
        {"emitter_init", 0x10D8E4, 7, {0x48, 0x8D, 0x0D, 0xD5, 0x2C, 0xB4, 0x05}, Part::Entities},
        {"emitter_vtable", 0x3100620, 7, {0x48, 0x8D, 0x05, 0xF1, 0x1D, 0x73, 0x00}, Part::Entities},
        {"ptfx_init", 0x11555A, 7, {0x48, 0x8D, 0x0D, 0xFF, 0x50, 0xB8, 0x05}, Part::Entities},
        {"ptfx_vtable", 0x115566, 7, {0x48, 0x8D, 0x05, 0x83, 0x97, 0x72, 0x03}, Part::Entities},
        {"script_init", 0x112D24, 7, {0x48, 0x8D, 0x0D, 0xF5, 0xEE, 0xB6, 0x05}, Part::Entities},
        {"script_vtable", 0x31B997D, 7, {0x48, 0x8D, 0x05, 0x24, 0x3B, 0x68, 0x00}, Part::Entities},
        {"music_ptr", 0xF5E3FD, 7, {0x48, 0x8B, 0x0D, 0xB4, 0xEE, 0xD1, 0x04}, Part::Entities},
        {"music_vtable", 0x316A4F7, 7, {0x48, 0x8D, 0x05, 0x4A, 0xD8, 0x6C, 0x00}, Part::Entities},
        {"music_tracks", 0x316FB78, 7, {0x48, 0x81, 0xC1, 0xA8, 0x06, 0x00, 0x00}, Part::Entities},
        {"music_tracks_end", 0x316FB7F, 7, {0x48, 0x8D, 0x91, 0x20, 0x07, 0x00, 0x00}, Part::Entities},
        {"music_track_sound", 0x316FB88, 4, {0x48, 0x8B, 0x41, 0x08}, Part::Entities},
        // The controls (weather, interior, exposure, speech duck, audio flags)
        {"wind_strength", 0x31CCC36, 8, {0xF3, 0x0F, 0x11, 0x97, 0xC4, 0x35, 0x00, 0x00}, Part::Env},
        {"wind_type_ovr", 0x31B8846, 7, {0x44, 0x38, 0xA6, 0xA8, 0x50, 0x00, 0x00}, Part::Env},
        {"wind_type_old", 0x31B884F, 6, {0x8B, 0x96, 0x9C, 0x50, 0x00, 0x00}, Part::Env},
        {"wind_type_new", 0x31B8864, 6, {0x8B, 0x96, 0xA0, 0x50, 0x00, 0x00}, Part::Env},
        {"wind_var_ovr", 0x31BDC88, 7, {0x80, 0xB9, 0x8C, 0x50, 0x00, 0x00, 0x00}, Part::Env},
        {"wind_var_value", 0x31BDC91, 8, {0xF3, 0x0F, 0x10, 0x81, 0x74, 0x50, 0x00, 0x00}, Part::Env},
        {"weather_scene", 0x31CEF79, 7, {0x48, 0x8D, 0x99, 0x68, 0x50, 0x00, 0x00}, Part::Env},
        {"weather_prev", 0x31B8744, 7, {0x44, 0x8B, 0x35, 0xB9, 0xDA, 0x3B, 0x01}, Part::Env},
        {"weather_next", 0x31B8737, 6, {0x8B, 0x05, 0xCB, 0xDA, 0x3B, 0x01}, Part::Env},
        {"weather_table", 0x31B873D, 7, {0x48, 0x8D, 0x15, 0x9C, 0x9B, 0x3B, 0x01}, Part::Env},
        {"weather_stride", 0x31B8752, 7, {0x49, 0x69, 0xCE, 0xE0, 0x01, 0x00, 0x00}, Part::Env},
        {"weather_hash", 0x31B8759, 4, {0x8B, 0x5C, 0x11, 0x10}, Part::Env},
        {"interior_obj", 0x31CCC13, 7, {0x48, 0x8D, 0x0D, 0xE6, 0x66, 0x9B, 0x02}, Part::Env},
        {"interior_in", 0x31D73ED, 7, {0x80, 0xB9, 0x3D, 0x34, 0x00, 0x00, 0x00}, Part::Env},
        {"interior_ratio", 0x31D7403, 6, {0x8B, 0x81, 0x44, 0x34, 0x00, 0x00}, Part::Env},
        {"exposure_obj", 0x31C6E7E, 7, {0x48, 0x8D, 0x0D, 0x0B, 0x09, 0xAD, 0x02}, Part::Env},
        {"exposure_dir", 0x31E1D9F, 9, {0xF3, 0x0F, 0x10, 0x84, 0x81, 0xF4, 0x0B, 0x00, 0x00}, Part::Env},
        {"duck_level", 0x2FCC2D9, 8, {0xF3, 0x0F, 0x11, 0x05, 0x43, 0xFC, 0xB8, 0x02}, Part::Env},
        {"duck_flag", 0x2F2D300, 7, {0x40, 0x38, 0x35, 0xD7, 0x5B, 0xC5, 0x02}, Part::Env},
        {"duck_scene", 0x2F2D255, 7, {0x48, 0x8B, 0x3D, 0x84, 0x8D, 0xC7, 0x02}, Part::Env},
        {"scene_hash_store", 0x2F19087, 6, {0x89, 0x8A, 0x28, 0x03, 0x00, 0x00}, Part::Env},
        {"scene_vars", 0x3279628, 4, {0x4C, 0x8D, 0x41, 0x18}, Part::Env},
        {"scene_var_count", 0x327962C, 4, {0x4D, 0x63, 0x50, 0x50}, Part::Env},
        {"scene_var_cap", 0x3279649, 4, {0x83, 0x79, 0x68, 0x0A}, Part::Env},
        {"flag_owner", 0x31C62BF, 7, {0x48, 0x8D, 0x0D, 0x5A, 0xB9, 0xAB, 0x02}, Part::Env},
        {"flag_counter", 0x31C6457, 8, {0x42, 0x8B, 0x84, 0x89, 0x6C, 0xCE, 0x00, 0x00}, Part::Env},
        {"flag_music_off", 0x317391D, 7, {0x44, 0x39, 0x3D, 0x6C, 0xB2, 0xB1, 0x02}, Part::Env},
        {"flag_table", 0x31C6243, 7, {0x4C, 0x8D, 0x0D, 0x96, 0x6B, 0x67, 0x00}, Part::Env},
        {"flag_table_end", 0x31C6251, 7, {0x48, 0x8D, 0x0D, 0xE8, 0x6C, 0x67, 0x00}, Part::Env},
        // The wrapper sound (the top of every
        // hoof sound, constructor 0x255CD94) adds the variable it resolved
        // for destination 0 to the combine volume every frame: Init 0x257A808
        // stores the resolved pointer at +0x108, ApplyVariables 0x2560D3C
        // reads it (below 0x400 an internal variable index, else a float*).
        // A sound's own variables are {name, value} pairs (0x25736B4), so the
        // name of a pointer into them is the dword before it.
        {"wrapper_vtable", 0x255CDA2, 7, {0x48, 0x8D, 0x05, 0x17, 0x24, 0x0B, 0x01}, Part::Wrapper},
        {"wrapper_volume_ref", 0x257A940, 7, {0x4C, 0x8D, 0xB7, 0x08, 0x01, 0x00, 0x00}, Part::Wrapper},
        {"wrapper_resolve", 0x257A955, 7, {0xFF, 0x50, 0x30, 0x49, 0x89, 0x06, 0x41}, Part::Wrapper},
        {"wrapper_apply", 0x2560D57, 7, {0x48, 0x8B, 0x81, 0x08, 0x01, 0x00, 0x00}, Part::Wrapper},
        {"wrapper_internal", 0x2560D61, 5, {0xBD, 0x00, 0x04, 0x00, 0x00}, Part::Wrapper},
        {"wrapper_add", 0x2560D79, 12, {0xF3, 0x0F, 0x10, 0x00, 0xF3, 0x0F, 0x58, 0x07, 0xF3, 0x0F, 0x11, 0x07}, Part::Wrapper},
        {"var_pair_scan", 0x2573717, 8, {0x39, 0x3A, 0x74, 0x16, 0x48, 0x83, 0xC2, 0x08}, Part::Wrapper},
        {"var_pair_value", 0x2573731, 4, {0x48, 0x8D, 0x42, 0x04}, Part::Wrapper},
        // Where the pairs live, so that a pointer is named "pair" only when
        // it falls inside the top sound's own pairs (else the dword before a
        // float of a global or of another block would be taken as its name).
        // 0x25736B4: index u16 [top+0x9C] (0xFFFF none), bucket
        // [sound+0xA0], slot = [pool + bucket * 0x2A860 + 0x2A848] + index *
        // [0x53588B0] (the sound pool's own layout), pair count i32 at
        // slot+0x80, pairs {name, value} from slot+0.
        {"var_req_index", 0x25736CD, 8, {0x44, 0x0F, 0xB7, 0x80, 0x9C, 0x00, 0x00, 0x00}, Part::Wrapper},
        {"var_req_bucket", 0x25736E0, 7, {0x0F, 0xB6, 0x83, 0xA0, 0x00, 0x00, 0x00}, Part::Wrapper},
        {"var_req_bucket_stride", 0x25736E7, 7, {0x48, 0x69, 0xC8, 0x60, 0xA8, 0x02, 0x00}, Part::Wrapper},
        {"var_req_pool", 0x25736EE, 7, {0x48, 0x8B, 0x05, 0xE3, 0x51, 0xDE, 0x02}, Part::Wrapper},
        {"var_req_stride", 0x25736F9, 7, {0x0F, 0xAF, 0x15, 0xB0, 0x51, 0xDE, 0x02}, Part::Wrapper},
        {"var_req_sounds", 0x2573700, 8, {0x48, 0x03, 0x94, 0x01, 0x48, 0xA8, 0x02, 0x00}, Part::Wrapper},
        {"var_req_count", 0x257370A, 7, {0x48, 0x63, 0x82, 0x80, 0x00, 0x00, 0x00}, Part::Wrapper},
    };
    constexpr std::size_t kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

    constexpr std::size_t SiteIndex(const char* name)
    {
        for (std::size_t n = 0; n < kSiteCount; ++n)
        {
            const char* a = kSites[n].name;
            const char* b = name;
            while (*a && *a == *b)
            {
                ++a;
                ++b;
            }
            if (*a == *b)
                return n;
        }
        return kSiteCount;
    }

    // rip-relative target of a site whose last 4 bytes are the displacement.
    constexpr std::uint32_t RipTarget(const Site& site)
    {
        const std::uint32_t at = site.size - 4u;
        const std::uint32_t d = static_cast<std::uint32_t>(site.bytes[at]) |
            (static_cast<std::uint32_t>(site.bytes[at + 1]) << 8) | (static_cast<std::uint32_t>(site.bytes[at + 2]) << 16) |
            (static_cast<std::uint32_t>(site.bytes[at + 3]) << 24);
        return site.rva + site.size + d;
    }
    constexpr std::uint32_t Rip(const char* name) { return RipTarget(kSites[SiteIndex(name)]); }
    // The little-endian 32-bit immediate (or displacement) at byte `at` of a site.
    constexpr std::uint32_t Imm32(const char* name, std::uint32_t at)
    {
        return static_cast<std::uint32_t>(kSites[SiteIndex(name)].bytes[at]) |
            (static_cast<std::uint32_t>(kSites[SiteIndex(name)].bytes[at + 1]) << 8) |
            (static_cast<std::uint32_t>(kSites[SiteIndex(name)].bytes[at + 2]) << 16) |
            (static_cast<std::uint32_t>(kSites[SiteIndex(name)].bytes[at + 3]) << 24);
    }
    static_assert(Imm32("bucket_stride", 3) == kBucketStride, "bucket_stride");
    static_assert(Imm32("list_bucket_stride", 3) == kBucketStride, "list_bucket_stride");
    // The pairs' slot is a slot of the same sound pool.
    static_assert(Imm32("var_req_bucket_stride", 3) == kBucketStride, "var_req_bucket_stride");
    static_assert(Imm32("var_req_sounds", 4) == kBucketSounds, "var_req_sounds");
    static_assert(Rip("var_req_pool") == kPoolTableRva, "var_req_pool");
    static_assert(Rip("var_req_stride") == kSoundStrideRva, "var_req_stride");
    static_assert(kSites[SiteIndex("var_req_bucket")].bytes[3] == kSoundBucket, "var_req_bucket");

    static_assert(Rip("cat_table") == kCategoryTableRva, "cat_table");
    static_assert(Rip("sound_stride") == kSoundStrideRva, "sound_stride");
    static_assert(Rip("pool_table") == kPoolTableRva, "pool_table");
    static_assert(Rip("slot_count") == kSlotCountRva, "slot_count");
    static_assert(Rip("bucket_count") == kBucketCountRva, "bucket_count");
    static_assert(Rip("settings_stride") == kSettingsStrideRva, "settings_stride");
    static_assert(Rip("audio_index") == kAudioIndexRva, "audio_index");
    static_assert(Rip("position_index") == kAudioIndexRva, "position_index");
    static_assert(Rip("env_sync") == kEnvSyncRva, "env_sync");
    static_assert(Rip("group_pool") == kGroupPoolRva, "group_pool");
    static_assert(Rip("group_cat_table") == kCategoryTableRva, "group_cat_table");
    static_assert(Rip("group_fade_scale") == kFadeScaleRva, "group_fade_scale");
    static_assert(Rip("scene_list_a") == kSceneListARva, "scene_list_a");
    static_assert(Rip("scene_count_a") == kSceneCountARva, "scene_count_a");
    static_assert(Rip("scene_list_b") == kSceneListBRva, "scene_list_b");
    static_assert(Rip("scene_count_b") == kSceneCountBRva, "scene_count_b");
    static_assert(Rip("quad_index") == kGameIndexRva, "quad_index");
    static_assert(Rip("weather_init") == kWeatherRva, "weather_init");
    static_assert(Rip("weather_vtable") == kWeatherVtableRva, "weather_vtable");
    static_assert(Rip("weather_wind") == kWeatherRva, "weather_wind");
    static_assert(Rip("ambient_init") == kAmbientRva, "ambient_init");
    static_assert(Rip("ambient_vtable") == kAmbientVtableRva, "ambient_vtable");
    static_assert(Rip("emitter_init") == kEmitterRva, "emitter_init");
    static_assert(Rip("emitter_vtable") == kEmitterVtableRva, "emitter_vtable");
    static_assert(Rip("ptfx_init") == kPtfxRva, "ptfx_init");
    static_assert(Rip("ptfx_vtable") == kPtfxVtableRva, "ptfx_vtable");
    static_assert(Rip("script_init") == kScriptAudioRva, "script_init");
    static_assert(Rip("script_vtable") == kScriptAudioVtableRva, "script_vtable");
    static_assert(Rip("music_ptr") == kMusicPtrRva, "music_ptr");
    static_assert(Rip("music_vtable") == kMusicVtableRva, "music_vtable");
    static_assert(Rip("weather_prev") == kWeatherPrevRva, "weather_prev");
    static_assert(Rip("weather_next") == kWeatherNextRva, "weather_next");
    static_assert(Rip("weather_table") == kWeatherTableRva, "weather_table");
    static_assert(Rip("interior_obj") == kInteriorRva, "interior_obj");
    static_assert(Rip("exposure_obj") == kExposureRva, "exposure_obj");
    static_assert(Rip("duck_level") == kDuckLevelRva, "duck_level");
    static_assert(Rip("duck_flag") == kDuckFlagRva, "duck_flag");
    static_assert(Rip("duck_scene") == kDuckSceneRva, "duck_scene");
    static_assert(Rip("flag_owner") == kScriptAudioRva, "flag_owner");
    static_assert(Rip("flag_music_off") == kAudioFlagCountersRva + 65 * 4, "flag_music_off");
    static_assert(Rip("flag_table") == kAudioFlagTableRva, "flag_table");
    static_assert(Rip("flag_table_end") == kAudioFlagTableRva + kAudioFlagCount * 4, "flag_table_end");
    static_assert(SiteIndex("no_such_site") == kSiteCount, "site lookup");

    inline bool SiteMatches(const Site& site, const std::uint8_t* code)
    {
        if (!code)
            return false;
        for (std::uint8_t n = 0; n < site.size; ++n)
            if (code[n] != site.bytes[n])
                return false;
        return true;
    }

    // Sanity bounds on the pool globals (a value outside: the layout is not
    // the one proven, nothing is walked).
    inline bool PoolSane(std::uint32_t buckets, std::uint32_t slots, std::uint32_t soundStride, std::uint32_t settingsStride)
    {
        return buckets >= 1 && buckets <= 64 && slots >= 32 && slots <= 65536 && soundStride >= kEnvBytes &&
            soundStride <= 0x2000 && settingsStride >= kSettingsMinBytes && settingsStride <= 0x2000;
    }
    inline std::uint32_t SettingsCopyBytes(std::uint32_t settingsStride)
    {
        return settingsStride < kSettingsBytes ? settingsStride : kSettingsBytes;
    }

    // ------------------------------------------------------------ the voice list

    // How many entries of a bucket's voice list can be used: the count read
    // before the copy (c1) and after it (c2). The frame zeroes the count at
    // its start (0x2539FAA) and bumps it per voice: c2 < c1 means a frame
    // started during the copy (partial). The caller copies again once.
    inline std::uint16_t ListUsable(std::uint16_t c1, std::uint16_t c2, bool& partial)
    {
        partial = c2 < c1;
        std::uint16_t n = c2 < c1 ? c2 : c1;
        if (n > kMaxVoicesPerBucket)
            n = kMaxVoicesPerBucket;
        return n;
    }

    // The list is rebuilt while the
    // frame processes the bucket's parents, OUTSIDE the bucket lock (the
    // frame takes [0x53588A0]+b*40 only around the zeroing and the parent
    // collection, 0x2539F83..0x253A007; the parents' 0x2533EF0 calls, which
    // register the voices through 0x2581400, run after the unlock). A copy
    // taken in that phase sees the first voices only and loses the rest of
    // the bucket for that snapshot, silently, so c2 > c1 ("grew") is not a
    // good copy. The count read again after the bucket's walk tells: the
    // same count is a settled list; more is a list being rebuilt (grew), less
    // is a new frame that zeroed it (reset). The walk repeats the bucket on
    // either and counts what it could not settle (list_growing).
    enum class ListChange : std::uint8_t { Settled, Grew, Reset };
    inline ListChange ListChangeOf(std::uint16_t before, std::uint16_t after)
    {
        if (after == before)
            return ListChange::Settled;
        return after > before ? ListChange::Grew : ListChange::Reset;
    }
    constexpr int kListAttempts = 3;

    enum class Entry : std::uint8_t
    {
        Ok,
        Hole,       // 0xFFFF: removed by the sound's destructor (0x255D492)
        OutOfRange, // slot past the bucket's slots
    };
    inline Entry EntryOf(std::uint16_t slot, std::uint32_t slots)
    {
        if (slot == kNoIndex)
            return Entry::Hole;
        return slot < slots ? Entry::Ok : Entry::OutOfRange;
    }

    // The link between entry i of a
    // bucket's voice list and its environment sound. The registration writes
    // i into E+0x242 (0x2567FF9); the frame's metric builder, called for the
    // same bucket right after (0x253A0B1 -> 0x256C3EC with r8b = 1), sets it
    // back to 0xFFFF once the voice is mixed (0x256D831, 0x256D896). So
    // between frames a mixed voice reads 0xFFFF (Mixed, the normal state), a
    // voice registered but not yet mixed reads i (Pending), and another index
    // means the sound was registered again in a newer list (Other: refused,
    // backlink_bad). The back link does not prove that a voice is counted
    // once: the walk keeps a per-bucket set of the sound slots it took
    // (SlotSet, list_dup).
    enum class Link : std::uint8_t { Pending, Mixed, Other };
    inline Link LinkOf(std::uint16_t envListIndex, std::uint16_t i)
    {
        if (envListIndex == i)
            return Link::Pending;
        if (envListIndex == kNoIndex)
            return Link::Mixed;
        return Link::Other;
    }

    // The sound slots one bucket walk took (a slot seen twice is counted once,
    // list_dup). Reused across buckets: no allocation once grown.
    class SlotSet
    {
    public:
        void Reset(std::uint32_t slots)
        {
            const std::size_t words = (static_cast<std::size_t>(slots) + 63u) / 64u;
            if (m_bits.size() < words)
                m_bits.resize(words);
            std::fill(m_bits.begin(), m_bits.begin() + static_cast<std::ptrdiff_t>(words), 0ull);
            m_slots = slots;
        }
        // True the first time a slot is taken; false when seen already or
        // out of range.
        bool Take(std::uint32_t slot)
        {
            if (slot >= m_slots)
                return false;
            std::uint64_t& word = m_bits[slot / 64u];
            const std::uint64_t bit = 1ull << (slot % 64u);
            if (word & bit)
                return false;
            word |= bit;
            return true;
        }

    private:
        std::vector<std::uint64_t> m_bits;
        std::uint32_t m_slots = 0;
    };

    // ------------------------------------------------------------ chain split

    // 0x25313D0: the buses that skip post-submix, category and groups.
    inline bool BusSkipsCategory(std::uint8_t busByte)
    {
        const std::uint32_t bus = busByte & 0x1Fu;
        return (bus >= 0x11 && bus <= 0x20) || bus == 0x22 || bus == 0x23;
    }

    // The engine's dB to linear for category volumes (0x256CC6A..0x256CC83).
    inline float CatLinear(float db)
    {
        if (!(db >= kCatFloorDb) || !std::isfinite(db))
            return 0.0f;
        return std::pow(10.0f, db * 0.05f);
    }

    // One mix group slot's factor (0x256CD2B..0x256CF2F): the previous group
    // fading out, the current group fading in. hasPrev/hasCur: the slot's
    // group resolves to a patched category (remap != -1).
    inline float GroupSlotFactor(bool bus, bool hasPrev, float prevDb, bool hasCur, float curDb, std::uint8_t fade)
    {
        if (bus)
            return 1.0f;
        const float f = static_cast<float>(fade) * kFadeScale;
        float out = 1.0f;
        if (hasPrev)
        {
            const float v = CatLinear(prevDb);
            out *= v + f * (1.0f - v);
        }
        if (hasCur)
        {
            const float v = CatLinear(curDb);
            out *= 1.0f + f * (v - 1.0f);
        }
        return out;
    }

    // A mixer scene as the chain's scene stage sees it.
    struct SceneInfo
    {
        std::uint64_t address = 0;
        std::uint16_t id = 0;
        float volume = 1.0f;
        std::int32_t mode = 0;
        std::int32_t tag = -1;
        std::uint32_t key = 0; // +0x2004
    };

    // 0x25316D0: a scene with tag -1 matches every sound; else the tag must be
    // one of the sound's tags (E+0x1AC, 6 s16, -1 none).
    inline bool SceneMatches(std::int32_t sceneTag, const std::int16_t* tags, std::size_t count)
    {
        if (sceneTag == -1)
            return true;
        for (std::size_t n = 0; n < count; ++n)
            if (tags[n] != -1 && static_cast<std::int32_t>(tags[n]) == sceneTag)
                return true;
        return false;
    }

    // 0x256D086..0x256D21A: the scenes of mode 0 multiply, then the scenes
    // of mode 1 take the minimum; the result multiplies the chain.
    inline float SceneTerm(const std::vector<const SceneInfo*>& scenes, const std::int16_t* tags, std::size_t tagCount)
    {
        float v = 1.0f;
        for (const SceneInfo* s : scenes)
            if (s && s->mode == 0 && SceneMatches(s->tag, tags, tagCount))
                v *= s->volume;
        for (const SceneInfo* s : scenes)
            if (s && s->mode == 1 && SceneMatches(s->tag, tags, tagCount))
                v = std::min(v, s->volume);
        return v;
    }

    // ------------------------------------------------------------ classes

    enum class Cls : std::uint8_t
    {
        SpeechScript = 0, // a speech line of type 2 or source 2 (script / conversation)
        SpeechAmbient,    // any other speech line (AI, ambient, bank)
        SpeechUnknown,    // a speech sound whose line was not found
        Footstep,         // steps of people and animals that are not horses
        Hoof,             // steps of horses (census caller family, horse owner)
        Horse,            // any other sound of a horse (tack, breath, vocal)
        Movement,         // ped movement foley
        Shot,             // the weapon's audio
        Melee,
        Owned,            // another sound of a recorded entity / puppet / the live player
        Wind,             // the weather entity's 12 wind slot sounds
        Weather,          // any other sound of the weather entity (rain, gusts, thunder)
        Ambient,          // the ambience entity (zones, rules)
        Emitter,          // the static emitter entity
        Ptfx,             // the particle emitter entity
        Music,            // the music manager's sounds and tracks
        Script,           // the script audio entity (script sounds, frontend by script)
        World,            // none of the above: no recorded owner, unknown entity
    };
    constexpr std::uint8_t kClsCount = 18;

    inline const char* ClsName(Cls cls)
    {
        static const char* const kNames[kClsCount] = {"speech_script", "speech_ambient", "speech_unknown", "footstep",
            "hoof", "horse", "movement", "shot", "melee", "owned", "wind", "weather", "ambient", "emitter", "ptfx",
            "music", "script", "world"};
        return static_cast<std::uint8_t>(cls) < kClsCount ? kNames[static_cast<std::uint8_t>(cls)] : "?";
    }

    // The classes of sounds without a recorded owner (VOLUME_PROBE_WORLD).
    inline bool WorldSide(Cls cls)
    {
        return cls == Cls::Wind || cls == Cls::Weather || cls == Cls::Ambient || cls == Cls::Emitter ||
            cls == Cls::Ptfx || cls == Cls::Music || cls == Cls::Script || cls == Cls::World;
    }

    // A speech line's class: the script path is type 2 (the five layer
    // conversation line) or source 2 (the recorded script lines); the
    // replay keeps the recorded type, so the type names the line
    // both ways even when the replay asks it as source 4.
    inline Cls SpeechCls(std::uint32_t source, std::uint32_t type)
    {
        return type == 2 || source == 2 ? Cls::SpeechScript : Cls::SpeechAmbient;
    }

    // A global audio entity of the image (RVA of the object, RVA of the vtable
    // its constructor writes) and the class of its sounds.
    struct GlobalEntity
    {
        const char* name;
        std::uint32_t rva;
        std::uint32_t vtableRva;
        Cls cls;
    };
    constexpr GlobalEntity kGlobalEntities[] = {
        {"weather", kWeatherRva, kWeatherVtableRva, Cls::Weather},
        {"ambient", kAmbientRva, kAmbientVtableRva, Cls::Ambient},
        {"emitter", kEmitterRva, kEmitterVtableRva, Cls::Emitter},
        {"ptfx", kPtfxRva, kPtfxVtableRva, Cls::Ptfx},
        {"script", kScriptAudioRva, kScriptAudioVtableRva, Cls::Script},
    };
    constexpr std::size_t kGlobalEntityCount = sizeof(kGlobalEntities) / sizeof(kGlobalEntities[0]);
    constexpr std::uint32_t kEntityWeatherBit = 1u; // kGlobalEntities[0]
    static_assert(kGlobalEntities[0].rva == kWeatherRva, "weather is entity 0");

    // The class of a top sound by its audio entity's RVA (proven: the bit of
    // the entity whose vtable matched at runtime). World when unknown.
    inline Cls ClsOfEntityRva(std::uint32_t rva, std::uint32_t provenMask)
    {
        for (std::size_t n = 0; n < kGlobalEntityCount; ++n)
            if (kGlobalEntities[n].rva == rva && (provenMask & (1u << n)))
                return kGlobalEntities[n].cls;
        return Cls::World;
    }

    // The census family of a born sound and its owner (horse or not).
    enum class BornKind : std::uint8_t { Shot, Footstep, HorseStep, Movement, Speech, Melee, Generic };
    inline Cls ClsOfBorn(BornKind kind, bool owned, bool horse)
    {
        switch (kind)
        {
        case BornKind::Shot: return Cls::Shot;
        case BornKind::Footstep: return horse ? Cls::Hoof : Cls::Footstep;
        case BornKind::HorseStep: return Cls::Hoof;
        case BornKind::Movement: return horse ? Cls::Horse : Cls::Movement;
        case BornKind::Speech: return Cls::SpeechUnknown;
        case BornKind::Melee: return Cls::Melee;
        default: return horse ? Cls::Horse : (owned ? Cls::Owned : Cls::World);
        }
    }

    // An address as an RVA of the image (in [base, end)), 0 for null, 1 for
    // anywhere else (heap).
    constexpr std::uint32_t kRvaNone = 0;
    constexpr std::uint32_t kRvaHeap = 1;
    inline std::uint32_t RvaOf(std::uint64_t address, std::uint64_t base, std::uint64_t end)
    {
        if (!address)
            return kRvaNone;
        if (base && address >= base && address < end)
            return static_cast<std::uint32_t>(address - base);
        return kRvaHeap;
    }
    // The variable the top wrapper sound adds
    // to its volume (sites wrapper_*). wvol=- (the top is not a wrapper),
    // none (no volume variable), int:<index> (an internal variable),
    // ped:<owner>:<name>=<value> (a float of a PED_VARIABLES block the probe
    // knows), pair:0x<name>=<value> (a {name, value} pair: the sound's own
    // variables), unreadable.
    // Pair only when the pointer is inside the top sound's
    // own pairs (sites var_req_*); any other float* is other=<value> (a
    // global, a block the last PED_VARS scan did not see), never a name read
    // from the dword before it.
    constexpr std::uint32_t kWrapperVtableRva = 0x360F1C0;
    constexpr std::uint32_t kWrapperVolumeRef = 0x108;
    constexpr std::uint64_t kInternalVariableLimit = 0x400;
    constexpr std::uint32_t kSoundVarIndex = 0x9C;  // u16, the sound's pairs slot (0xFFFF none)
    constexpr std::uint32_t kVarPairCount = 0x80;   // i32 in that slot
    constexpr std::uint32_t kVarPairStride = 8;     // {u32 name, f32 value}
    constexpr std::int32_t kMaxVarPairs = static_cast<std::int32_t>(kVarPairCount / kVarPairStride);
    static_assert(kSites[SiteIndex("var_req_index")].bytes[4] == kSoundVarIndex, "var_req_index");
    static_assert(kSites[SiteIndex("var_req_count")].bytes[3] == kVarPairCount, "var_req_count");
    // The pointer is the value of pair n of a slot holding count pairs.
    inline bool PairSlotHit(std::uint64_t slot, std::int32_t count, std::uint64_t ref)
    {
        if (!slot || count <= 0 || count > kMaxVarPairs || ref < slot + 4)
            return false;
        const std::uint64_t off = ref - slot;
        return off % kVarPairStride == 4 && off / kVarPairStride < static_cast<std::uint64_t>(count);
    }
    enum class VarHome : std::uint8_t { NotWrapper, None, Internal, Ped, Pair, Unreadable, Other };
    struct WrapperVar
    {
        VarHome home = VarHome::NotWrapper;
        std::uint64_t ref = 0;
        std::string owner;            // Ped
        std::uint32_t name = 0;       // Ped, Pair
        const char* label = nullptr;  // Ped: a known PED_VARIABLES name
        float value = 0.0f;           // Ped, Pair
    };
    inline std::string WrapperVarText(const WrapperVar& w)
    {
        char text[128];
        switch (w.home)
        {
        case VarHome::NotWrapper: return "-";
        case VarHome::None: return "none";
        case VarHome::Unreadable: return "unreadable";
        case VarHome::Internal:
            std::snprintf(text, sizeof(text), "int:%llu", static_cast<unsigned long long>(w.ref));
            return text;
        case VarHome::Ped:
            if (w.label)
                std::snprintf(text, sizeof(text), "ped:%s:%s=%.2f", w.owner.c_str(), w.label, w.value);
            else
                std::snprintf(text, sizeof(text), "ped:%s:0x%08x=%.2f", w.owner.c_str(), w.name, w.value);
            return text;
        case VarHome::Pair:
            std::snprintf(text, sizeof(text), "pair:0x%08x=%.2f", w.name, w.value);
            return text;
        case VarHome::Other:
            std::snprintf(text, sizeof(text), "other=%.2f", w.value);
            return text;
        }
        return "?";
    }

    inline std::string RvaText(std::uint32_t rva)
    {
        if (rva == kRvaNone)
            return "none";
        if (rva == kRvaHeap)
            return "heap";
        char b[24];
        std::snprintf(b, sizeof(b), "0x%x", rva);
        return b;
    }

    // ------------------------------------------------------------ math

    constexpr float kSilentDb = -100.0f;

    inline float LinToDb(float linear)
    {
        if (!(linear > 1e-5f) || !std::isfinite(linear))
            return kSilentDb;
        return 20.0f * std::log10(linear);
    }

    inline float PowerToDb(double power)
    {
        if (!(power > 1e-10) || !std::isfinite(power))
            return kSilentDb;
        return static_cast<float>(10.0 * std::log10(power));
    }

    // The power of 4 linear speaker levels (sum of squares), in dB.
    inline float QuadPowerDb(const float levels[4])
    {
        double p = 0.0;
        for (int n = 0; n < 4; ++n)
            if (std::isfinite(levels[n]))
                p += static_cast<double>(levels[n]) * static_cast<double>(levels[n]);
        return PowerToDb(p);
    }

    inline bool Finite3(const float p[3]) { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }
    inline bool Zero3(const float p[3]) { return p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f; }
    inline float Dist3(const float a[3], const float b[3])
    {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // ------------------------------------------------------------ one voice

    // What the probe read for one audEnvironmentSound in one snapshot.
    struct Voice
    {
        float final = 0.0f;     // E+0x224 linear
        float volume = 0.0f;    // V+0xD0 linear (biased)
        float chain = 0.0f;     // V+0xD4 linear
        float reqVolumeDb = 0.0f;
        float postDb = 0.0f;
        float dry = 1.0f;
        float wet = 0.0f;
        float catDb = 0.0f;
        bool catKnown = false;
        std::int32_t category = -1;
        float distM = -1.0f;    // to the nearest volume point; -1 not positional / unknown
        float open = -1.0f;     // occlusion open factor (S+0xD4) when an environment group is set; -1 else
        std::uint8_t mixGroups = 0; // non-0xFF current mix group bytes
        Cls cls = Cls::World;
        bool voiceRead = false; // V was readable
        float combDb = 0.0f;    // combine buffer +0: the hierarchy's combined volume (dB)
        float combPostDb = 0.0f;
        bool combRead = false;
        bool bus = false;       // the bus skips post, category and groups
        float group = 1.0f;     // the mix group term (linear) the engine multiplies
        bool groupKnown = false;
        float scene = 1.0f;     // the mixer scene term (linear)
        bool sceneKnown = false;
        std::uint8_t scenes = 0; // scene ids on the sound
        bool quad = false;      // quad speaker levels (no position)
        float quadDb = 0.0f;    // their power (dB)
        std::uint8_t posMode = 0;
        bool playing = true;    // +0xF2 state 2
        std::uint32_t entityRva = kRvaNone; // the top sound's audio entity
        std::uint32_t topVtRva = kRvaNone;  // the top sound's vtable
    };

    // Derived stages (dB). final ~= volume + wetdry + chain; resid = chain -
    // post - category (both 0 on a skipping bus) = mix groups + scenes +
    // occlusion + distance attenuation; rest = resid - groups - scenes.
    //
    // The post the chain multiplies is
    // the COMBINE BUFFER's (+4: the requested post-submix S+0x124 plus what
    // the hierarchy adds, 0x256F514/0x256F58B), not S+0x124 alone: the
    // metric builder loads it at 0x256C738 (movss xmm8,[r9+4], r9 the
    // combine the frame passes), makes it linear at 0x256C861..0x256C879
    // ([rbp+0x19C]) and the category multiplies that (0x256CC86); S+0x124
    // is not read in 0x256C3EC at all. Subtracting S+0x124 would put a
    // post-submix attenuation of the sound's metadata (or a different one on
    // the sound the replay asks) into resid_db and rest_db, read as
    // occlusion or distance. AppliedPostDb is the post the engine applied
    // (0 on a skipping bus, 0x256C762); AppliedCatDb the category (0 on a
    // skipping bus, 0x256CC5B).
    inline float AppliedPostDb(const Voice& v)
    {
        if (v.bus)
            return 0.0f;
        return v.combRead ? v.combPostDb : v.postDb;
    }
    inline float AppliedCatDb(const Voice& v)
    {
        return v.bus || !v.catKnown ? 0.0f : v.catDb;
    }

    struct Stages
    {
        float finalDb = kSilentDb;
        float volumeDb = kSilentDb;
        float chainDb = kSilentDb;
        float wetDryDb = 0.0f;
        float postDb = 0.0f;     // the post the chain applied (AppliedPostDb)
        float catDb = 0.0f;      // the category the chain applied (AppliedCatDb)
        float residDb = 0.0f;
        float groupDb = 0.0f;
        float sceneDb = 0.0f;
        float restDb = 0.0f;
        float effDb = kSilentDb; // final plus the quad levels' power
        bool consistent = false; // final within 0.5 dB of volume + wetdry + chain (or both silent)
    };

    inline Stages StagesOf(const Voice& v)
    {
        Stages s;
        s.finalDb = LinToDb(v.final);
        s.effDb = v.quad && s.finalDb > kSilentDb ? s.finalDb + v.quadDb : s.finalDb;
        if (s.effDb < kSilentDb)
            s.effDb = kSilentDb;
        if (!v.voiceRead)
            return s;
        s.volumeDb = v.volume > 1e-5f ? LinToDb(v.volume) + kVolumeBiasDb : kSilentDb;
        s.chainDb = LinToDb(v.chain);
        s.wetDryDb = LinToDb(v.dry + v.wet);
        s.postDb = AppliedPostDb(v);
        s.catDb = AppliedCatDb(v);
        s.residDb = s.chainDb - s.postDb - s.catDb;
        s.groupDb = v.groupKnown ? LinToDb(v.group) : 0.0f;
        s.sceneDb = v.sceneKnown ? LinToDb(v.scene) : 0.0f;
        s.restDb = s.residDb - s.groupDb - s.sceneDb;
        const float sum = s.volumeDb + s.wetDryDb + s.chainDb;
        s.consistent = (s.finalDb <= kSilentDb + 1.0f && (s.volumeDb <= kSilentDb + 1.0f || s.chainDb <= kSilentDb + 1.0f)) ||
            std::fabs(sum - s.finalDb) <= 0.5f;
        return s;
    }

    // ------------------------------------------------------------ windows

    // Per class (or per category, or per world key) and window: power sum
    // per snapshot, the loudest voice, power-weighted stage means.
    struct Cell
    {
        std::uint64_t n = 0;     // voices summed over the snapshots
        double power = 0.0;      // sum of final^2
        double effPower = 0.0;   // sum of final^2 * quad power
        float maxDb = kSilentDb;
        double wSum = 0.0;       // weights (final^2) of the stage means
        double wVol = 0.0, wReq = 0.0, wPost = 0.0, wCat = 0.0, wChain = 0.0, wResid = 0.0, wWetDry = 0.0;
        double wComb = 0.0, wCombW = 0.0;
        double wGroup = 0.0, wScene = 0.0, wRest = 0.0;
        double wDist = 0.0, wDistW = 0.0; // distance mean over positional voices
        double wOpen = 0.0, wOpenW = 0.0; // open mean over voices with an environment group
        double wQuad = 0.0, wQuadW = 0.0; // quad power mean over quad voices
        std::uint64_t mixGrouped = 0;
        std::uint64_t inconsistent = 0;
        std::uint64_t quadVoices = 0;
        std::uint64_t positional = 0;
        std::uint64_t busVoices = 0;
        std::uint64_t scened = 0;
        // Unweighted fallback (all voices silent).
        double uVol = 0.0, uChain = 0.0, uCat = 0.0;
        std::uint64_t uN = 0;
        std::int32_t topCategory = -1; // the category of the loudest voice
        Cls topCls = Cls::World;

        void Add(const Voice& v, const Stages& s)
        {
            ++n;
            const double p = static_cast<double>(v.final) * static_cast<double>(v.final);
            if (std::isfinite(p))
            {
                power += p;
                const double q = v.quad ? std::pow(10.0, static_cast<double>(v.quadDb) / 10.0) : 1.0;
                if (std::isfinite(q))
                    effPower += p * q;
            }
            if (s.finalDb > maxDb)
            {
                maxDb = s.finalDb;
                topCategory = v.category;
                topCls = v.cls;
            }
            mixGrouped += v.mixGroups ? 1 : 0;
            inconsistent += s.consistent ? 0 : 1;
            quadVoices += v.quad ? 1 : 0;
            positional += v.distM >= 0.0f ? 1 : 0;
            busVoices += v.bus ? 1 : 0;
            scened += v.scenes ? 1 : 0;
            if (v.voiceRead)
            {
                ++uN;
                uVol += s.volumeDb;
                uChain += s.chainDb;
                uCat += s.catDb;
            }
            if (!(p > 0.0) || !std::isfinite(p) || !v.voiceRead)
                return;
            wSum += p;
            wVol += p * s.volumeDb;
            wReq += p * v.reqVolumeDb;
            wPost += p * s.postDb; // the applied post (combine), not S+0x124
            wCat += p * s.catDb;   // 0 on a skipping bus (not applied)
            wChain += p * s.chainDb;
            wResid += p * s.residDb;
            wWetDry += p * s.wetDryDb;
            wGroup += p * s.groupDb;
            wScene += p * s.sceneDb;
            wRest += p * s.restDb;
            if (v.combRead)
            {
                wComb += p * v.combDb;
                wCombW += p;
            }
            if (v.distM >= 0.0f)
            {
                wDist += p * v.distM;
                wDistW += p;
            }
            if (v.open >= 0.0f)
            {
                wOpen += p * v.open;
                wOpenW += p;
            }
            if (v.quad)
            {
                wQuad += p * v.quadDb;
                wQuadW += p;
            }
        }
    };

    // A world-side group: the top sound's entity, its type (vtable) and the
    // engine category.
    struct WorldKey
    {
        std::uint32_t entity = kRvaNone;
        std::uint32_t topVt = kRvaNone;
        std::int32_t category = -1;
        bool operator<(const WorldKey& o) const
        {
            return std::tie(entity, topVt, category) < std::tie(o.entity, o.topVt, o.category);
        }
    };

    struct WindowInfo
    {
        std::uint32_t snaps = 0;      // snapshots in the window
        std::uint64_t voices = 0;     // voices seen (all classes)
        std::uint64_t unmatched = 0;  // voices of class world
        float listener[3] = {};
        bool hasListener = false;
        double lstPlayer = 0.0, lstCam = 0.0;
        std::uint32_t lstN = 0;
        std::uint32_t lstPlayerN = 0; // snapshots with a live player (the mean is over these only)
        // The volume points of the
        // listener (summed over the snapshots) and the last second point.
        std::uint64_t lstPoints = 0;
        float listener1[3] = {};
        bool hasListener1 = false;
        std::uint64_t idle = 0;       // voices in the list whose sound is not in state 2 (counted, kept)
        std::uint32_t music = 0;      // snapshots with AUDIO_IS_MUSIC_PLAYING
        std::uint32_t envSync = 0;    // snapshots with the env sync byte set
        // The walk's evidence
        std::uint64_t listVoices = 0;   // entries of the voice lists (all buckets, summed over snapshots)
        std::uint64_t tops = 0;         // parent bits set
        std::uint64_t topsEnv = 0;      // of them environment sounds
        std::uint64_t listPartial = 0;  // copies where the frame reset the list
        std::uint64_t backlinkBad = 0;  // entries whose sound points to another entry (Link::Other)
        std::uint64_t linkMixed = 0;    // entries kept whose sound reads 0xFFFF (mixed this frame)
        std::uint64_t linkPending = 0;  // entries kept whose sound reads i (not mixed yet)
        std::uint64_t listDup = 0;      // entries whose sound slot the bucket walk took already
        std::uint64_t listNotEnv = 0;   // entries whose sound is not an environment sound
        std::uint64_t holes = 0;        // 0xFFFF entries
        std::uint64_t settingsMismatch = 0; // E+0xD8 != top +0xD6
        std::uint64_t scenesSeen = 0;   // mixer scenes in the lists (summed over snapshots)
        // Bucket walks repeated because the list changed under them
        // (ListChangeOf), and buckets left unsettled after kListAttempts (a
        // list still being rebuilt: that snapshot misses part of the bucket).
        std::uint64_t listRetried = 0;
        std::uint64_t listGrowing = 0;
    };

    class Window
    {
    public:
        std::int64_t index = -1; // clip window number (-1: none open)
        WindowInfo info;
        Cell cls[kClsCount];
        Cell all;
        std::map<std::int32_t, Cell> categories;
        std::map<WorldKey, Cell> world;

        void Reset(std::int64_t newIndex)
        {
            index = newIndex;
            info = WindowInfo{};
            for (Cell& c : cls)
                c = Cell{};
            all = Cell{};
            categories.clear();
            world.clear();
        }
        void Add(const Voice& v)
        {
            const Stages s = StagesOf(v);
            cls[static_cast<std::uint8_t>(v.cls) < kClsCount ? static_cast<std::uint8_t>(v.cls) : kClsCount - 1].Add(v, s);
            all.Add(v, s);
            categories[v.category].Add(v, s);
            if (WorldSide(v.cls))
            {
                WorldKey key;
                key.entity = v.entityRva;
                key.topVt = v.topVtRva;
                key.category = v.category;
                world[key].Add(v, s);
            }
            ++info.voices;
        }
        bool Empty() const { return index < 0 || info.snaps == 0; }
    };

    // Which window a clip time falls in.
    inline std::int64_t WindowOf(double clipMs, std::uint32_t windowMs)
    {
        if (windowMs == 0)
            windowMs = 500;
        if (!(clipMs >= 0.0))
            return 0;
        return static_cast<std::int64_t>(clipMs / static_cast<double>(windowMs));
    }

    // A pass's segments: a window index lower than the last one (a loop or a
    // seek back) opens a new segment, so the second loop of a replay does not
    // overwrite the first one's windows in the diff.
    struct Segments
    {
        std::int64_t last = -1;
        std::uint32_t seg = 0;
        void Reset()
        {
            last = -1;
            seg = 0;
        }
        std::uint32_t Next(std::int64_t index)
        {
            if (last >= 0 && index < last)
                ++seg;
            last = index;
            return seg;
        }
    };

    inline std::string Fmt1(double v)
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%.1f", v);
        return b;
    }
    inline std::string Fmt2(double v)
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%.2f", v);
        return b;
    }
    inline std::string Fmt3(double v)
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%.3f", v);
        return b;
    }
    inline std::string Hex(std::uint64_t v)
    {
        char b[24];
        std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(v));
        return b;
    }

    // The stage fields of a cell ("na" when nothing was audible and readable).
    inline std::string StageFields(const Cell& c)
    {
        std::string out;
        if (c.wSum > 0.0)
        {
            out += ",vol_db=" + Fmt1(c.wVol / c.wSum) + ",req_db=" + Fmt1(c.wReq / c.wSum) + ",post_db=" + Fmt1(c.wPost / c.wSum) +
                ",cat_db=" + Fmt1(c.wCat / c.wSum) + ",chain_db=" + Fmt1(c.wChain / c.wSum) + ",resid_db=" +
                Fmt1(c.wResid / c.wSum) + ",wetdry_db=" + Fmt1(c.wWetDry / c.wSum);
            out += ",grp_db=" + Fmt1(c.wGroup / c.wSum) + ",scn_db=" + Fmt1(c.wScene / c.wSum) + ",rest_db=" +
                Fmt1(c.wRest / c.wSum);
        }
        else if (c.uN)
        {
            out += ",vol_db=" + Fmt1(c.uVol / c.uN) + ",req_db=na,post_db=na,cat_db=" + Fmt1(c.uCat / c.uN) + ",chain_db=" +
                Fmt1(c.uChain / c.uN) + ",resid_db=na,wetdry_db=na,grp_db=na,scn_db=na,rest_db=na";
        }
        else
            out += ",vol_db=na,req_db=na,post_db=na,cat_db=na,chain_db=na,resid_db=na,wetdry_db=na,grp_db=na,scn_db=na,rest_db=na";
        out += ",comb_db=" + (c.wCombW > 0.0 ? Fmt1(c.wComb / c.wCombW) : std::string("na"));
        out += ",dist_m=" + (c.wDistW > 0.0 ? Fmt1(c.wDist / c.wDistW) : std::string("na"));
        out += ",open=" + (c.wOpenW > 0.0 ? Fmt2(c.wOpen / c.wOpenW) : std::string("na"));
        out += ",quad_db=" + (c.wQuadW > 0.0 ? Fmt1(c.wQuad / c.wQuadW) : std::string("na"));
        return out;
    }

    // The shares of a cell's voices (quad levels, positional, skipping bus, with scenes).
    inline std::string ShareFields(const Cell& c)
    {
        const double n = c.n ? static_cast<double>(c.n) : 1.0;
        return ",mg=" + Fmt2(c.n ? static_cast<double>(c.mixGrouped) / n : 0.0) + ",quad=" +
            Fmt2(c.n ? static_cast<double>(c.quadVoices) / n : 0.0) + ",positional=" +
            Fmt2(c.n ? static_cast<double>(c.positional) / n : 0.0) + ",bus_skip=" +
            Fmt2(c.n ? static_cast<double>(c.busVoices) / n : 0.0) + ",scened=" +
            Fmt2(c.n ? static_cast<double>(c.scened) / n : 0.0);
    }

    inline std::string Head(const char* tag, const char* phase, std::uint32_t pass, std::uint32_t seg, std::uint32_t clipMs)
    {
        return std::string(tag) + ",phase=" + phase + ",pass=" + std::to_string(pass) + ",seg=" + std::to_string(seg) +
            ",clip_ms=" + std::to_string(clipMs);
    }

    // VOLUME_PROBE line of one class (or "all") of a closed window.
    inline std::string ClassLine(const char* phase, std::uint32_t pass, std::uint32_t seg, std::uint32_t clipMs,
        std::uint32_t windowMs, const char* name, const Cell& c, const WindowInfo& info)
    {
        const double snaps = info.snaps ? static_cast<double>(info.snaps) : 1.0;
        std::string line = Head("VOLUME_PROBE", phase, pass, seg, clipMs) + ",win_ms=" + std::to_string(windowMs) +
            ",cat=" + name + ",snaps=" + std::to_string(info.snaps) + ",count=" + Fmt2(static_cast<double>(c.n) / snaps) +
            ",sum_db=" + Fmt1(PowerToDb(c.power / snaps)) + ",eff_db=" + Fmt1(PowerToDb(c.effPower / snaps)) +
            ",max_db=" + Fmt1(c.maxDb);
        line += StageFields(c);
        line += ShareFields(c);
        line += ",top_category=" + std::to_string(c.topCategory);
        if (info.hasListener)
        {
            char lst[96];
            std::snprintf(lst, sizeof(lst), ",listener_pos=%.1f;%.1f;%.1f", info.listener[0], info.listener[1], info.listener[2]);
            line += lst;
        }
        else
            line += ",listener_pos=na";
        line += ",listener_to_player_m=" + (info.lstPlayerN ? Fmt1(info.lstPlayer / info.lstPlayerN) : std::string("na"));
        line += ",listener_to_cam_m=" + (info.lstN ? Fmt1(info.lstCam / info.lstN) : std::string("na"));
        // How many volume points the listener had (mean) and the second one.
        line += ",listener_points=" + (info.lstN ? Fmt2(static_cast<double>(info.lstPoints) / info.lstN) : std::string("na"));
        if (info.hasListener1)
        {
            char pos1Text[96];
            std::snprintf(pos1Text, sizeof(pos1Text), ",listener_pos1=%.1f;%.1f;%.1f", info.listener1[0], info.listener1[1],
                info.listener1[2]);
            line += pos1Text;
        }
        else
            line += ",listener_pos1=na";
        line += ",inconsistent=" + std::to_string(c.inconsistent);
        return line;
    }

    // VOLUME_PROBE_CATEGORY: one engine category of a closed window.
    inline std::string CategoryLine(const char* phase, std::uint32_t pass, std::uint32_t seg, std::uint32_t clipMs,
        std::int32_t category, const Cell& c, const WindowInfo& info)
    {
        const double snaps = info.snaps ? static_cast<double>(info.snaps) : 1.0;
        std::string line = Head("VOLUME_PROBE_CATEGORY", phase, pass, seg, clipMs) + ",category=" +
            std::to_string(category) + ",count=" + Fmt2(static_cast<double>(c.n) / snaps) + ",sum_db=" +
            Fmt1(PowerToDb(c.power / snaps)) + ",eff_db=" + Fmt1(PowerToDb(c.effPower / snaps)) + ",max_db=" +
            Fmt1(c.maxDb) + ",top_cls=" + ClsName(c.topCls);
        line += StageFields(c);
        line += ShareFields(c);
        return line;
    }

    // VOLUME_PROBE_WORLD: one world-side group (entity, top type, category).
    inline std::string WorldLine(const char* phase, std::uint32_t pass, std::uint32_t seg, std::uint32_t clipMs,
        const WorldKey& key, const Cell& c, const WindowInfo& info)
    {
        const double snaps = info.snaps ? static_cast<double>(info.snaps) : 1.0;
        std::string line = Head("VOLUME_PROBE_WORLD", phase, pass, seg, clipMs) + ",ent=" + RvaText(key.entity) +
            ",top_vt=" + RvaText(key.topVt) + ",category=" + std::to_string(key.category) + ",cls=" + ClsName(c.topCls) +
            ",count=" + Fmt2(static_cast<double>(c.n) / snaps) + ",sum_db=" + Fmt1(PowerToDb(c.power / snaps)) +
            ",eff_db=" + Fmt1(PowerToDb(c.effPower / snaps)) + ",max_db=" + Fmt1(c.maxDb);
        line += StageFields(c);
        line += ShareFields(c);
        return line;
    }

    // The categories of a window by power, loudest first, at most `top`.
    inline std::vector<std::int32_t> TopCategories(const Window& w, std::size_t top)
    {
        std::vector<std::pair<double, std::int32_t>> order;
        for (const auto& entry : w.categories)
            order.push_back({entry.second.power, entry.first});
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        std::vector<std::int32_t> out;
        for (std::size_t n = 0; n < order.size() && n < top; ++n)
            out.push_back(order[n].second);
        return out;
    }

    // The world groups of a window by effective power, loudest first.
    inline std::vector<WorldKey> TopWorld(const Window& w, std::size_t top)
    {
        std::vector<std::pair<double, WorldKey>> order;
        for (const auto& entry : w.world)
            order.push_back({entry.second.effPower, entry.first});
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        std::vector<WorldKey> out;
        for (std::size_t n = 0; n < order.size() && n < top; ++n)
            out.push_back(order[n].second);
        return out;
    }

    // ------------------------------------------------------------ the controls

    // One snapshot of the controls suspected of the loud wind (all optional:
    // `have` says which parts were read).
    struct EnvSample
    {
        bool weather = false;      // the weather entity matched its vtable
        float windStrength = 0.0f; // W+0x4F44
        std::uint8_t windTypeOverride = 0;
        std::uint32_t windTypeOld = 0, windTypeNew = 0;
        std::uint8_t windVarOverride = 0;
        float windVarValue = 0.0f;
        std::uint32_t weatherPrev = 0, weatherNext = 0; // type hashes
        bool weatherScene = false;       // the weather mixer scene (W+0x5068)
        std::uint32_t weatherSceneHash = 0;
        float weatherSceneApply = -1.0f; // its "apply" variable (-1 none)
        std::uint32_t windSlots = 0;      // non-null wind slot sounds
        std::uint32_t windSlotsPlaying = 0;
        bool interiorRead = false;
        std::uint8_t interior = 0;
        float interiorRatio = 0.0f;
        bool exposureRead = false;
        float exposure[4] = {};
        bool duckRead = false;
        std::uint8_t duckFlag = 0;
        float duckLevel = 0.0f;
        bool duckScene = false;          // the conversation duck scene (0x5BA5FE0)
        std::uint32_t duckSceneHash = 0;
        float duckSceneApply = -1.0f;
        float duckSceneWeather = -1.0f;  // its weatherApply variable (-1 none)
        float duckSceneSfx = -1.0f;      // its sfxApply variable (-1 none)
        bool flagsRead = false;
        std::int32_t flags[kAudioFlagCount] = {};
        bool music = false;               // the music manager matched its vtable
        std::uint32_t musicTracks = 0;    // track slots with a playing sound
    };

    // Window accumulation of EnvSample.
    struct EnvWindow
    {
        std::uint32_t n = 0;
        std::uint32_t weatherN = 0;
        double windStrength = 0.0;
        float windStrengthMax = 0.0f;
        EnvSample last;
        std::uint32_t interiorN = 0, interiorIn = 0;
        double interiorRatio = 0.0;
        std::uint32_t exposureN = 0;
        double exposure[4] = {};
        std::uint32_t duckN = 0, duckOn = 0;
        double duckLevel = 0.0;
        float duckLevelMax = 0.0f;
        std::uint32_t weatherSceneN = 0;
        double weatherSceneApply = 0.0;
        std::uint32_t duckSceneN = 0;
        double duckSceneApply = 0.0;
        std::uint32_t duckWeatherN = 0, duckSfxN = 0;
        double duckWeather = 0.0, duckSfx = 0.0;
        std::int32_t flagsMax[kAudioFlagCount] = {};
        bool flagsRead = false;
        std::uint32_t windSlotsMax = 0, windPlayingMax = 0, musicTracksMax = 0;

        void Reset() { *this = EnvWindow{}; }
        void Add(const EnvSample& s)
        {
            ++n;
            last = s;
            if (s.weather)
            {
                ++weatherN;
                windStrength += s.windStrength;
                windStrengthMax = std::max(windStrengthMax, s.windStrength);
                if (s.weatherScene)
                {
                    ++weatherSceneN;
                    weatherSceneApply += s.weatherSceneApply;
                }
                windSlotsMax = std::max(windSlotsMax, s.windSlots);
                windPlayingMax = std::max(windPlayingMax, s.windSlotsPlaying);
            }
            if (s.interiorRead)
            {
                ++interiorN;
                interiorIn += s.interior ? 1u : 0u;
                interiorRatio += s.interiorRatio;
            }
            if (s.exposureRead)
            {
                ++exposureN;
                for (int d = 0; d < 4; ++d)
                    exposure[d] += s.exposure[d];
            }
            if (s.duckRead)
            {
                ++duckN;
                duckOn += s.duckFlag ? 1u : 0u;
                duckLevel += s.duckLevel;
                duckLevelMax = std::max(duckLevelMax, s.duckLevel);
                if (s.duckScene)
                {
                    ++duckSceneN;
                    duckSceneApply += s.duckSceneApply;
                    if (s.duckSceneWeather >= 0.0f)
                    {
                        ++duckWeatherN;
                        duckWeather += s.duckSceneWeather;
                    }
                    if (s.duckSceneSfx >= 0.0f)
                    {
                        ++duckSfxN;
                        duckSfx += s.duckSceneSfx;
                    }
                }
            }
            if (s.flagsRead)
            {
                flagsRead = true;
                for (std::uint32_t f = 0; f < kAudioFlagCount; ++f)
                    flagsMax[f] = std::max(flagsMax[f], s.flags[f]);
            }
            musicTracksMax = std::max(musicTracksMax, s.musicTracks);
        }
    };

    // The audio flags set in a window: "index:hash:count;..." (none).
    inline std::string FlagsText(const std::int32_t* counts, const std::uint32_t* hashes, std::uint32_t count)
    {
        std::string out;
        for (std::uint32_t f = 0; f < count; ++f)
        {
            if (counts[f] <= 0)
                continue;
            char b[48];
            std::snprintf(b, sizeof(b), "%s%u:0x%08x:%d", out.empty() ? "" : ";", f, hashes ? hashes[f] : 0u, counts[f]);
            out += b;
        }
        return out.empty() ? std::string("none") : out;
    }

    // VOLUME_PROBE_ENV: the controls of a closed window.
    inline std::string EnvLine(const char* phase, std::uint32_t pass, std::uint32_t seg, std::uint32_t clipMs,
        const EnvWindow& e, const std::uint32_t* flagHashes, std::uint32_t scenes, const std::string& lowScenes)
    {
        std::string line = Head("VOLUME_PROBE_ENV", phase, pass, seg, clipMs) + ",snaps=" + std::to_string(e.n);
        const EnvSample& l = e.last;
        if (e.weatherN)
        {
            line += ",weather=1,wind_str=" + Fmt3(e.windStrength / e.weatherN) + ",wind_str_max=" + Fmt3(e.windStrengthMax) +
                ",wind_type_ovr=" + std::to_string(l.windTypeOverride) + ",wind_type=" + Hex(l.windTypeOld) + ";" +
                Hex(l.windTypeNew) + ",wvar_ovr=" + std::to_string(l.windVarOverride) + ",wvar=" + Fmt3(l.windVarValue) +
                ",wx_prev=" + Hex(l.weatherPrev) + ",wx_next=" + Hex(l.weatherNext) + ",wx_scene=" +
                (e.weatherSceneN ? Hex(l.weatherSceneHash) + ":" + Fmt3(e.weatherSceneApply / e.weatherSceneN)
                                 : std::string("none")) +
                ",wind_slots=" + std::to_string(e.windSlotsMax) + ",wind_playing=" + std::to_string(e.windPlayingMax);
        }
        else
            line += ",weather=0";
        if (e.interiorN)
            line += ",interior=" + Fmt2(static_cast<double>(e.interiorIn) / e.interiorN) + ",interior_ratio=" +
                Fmt3(e.interiorRatio / e.interiorN);
        else
            line += ",interior=na,interior_ratio=na";
        if (e.exposureN)
            line += ",expo=" + Fmt3(e.exposure[0] / e.exposureN) + ";" + Fmt3(e.exposure[1] / e.exposureN) + ";" +
                Fmt3(e.exposure[2] / e.exposureN) + ";" + Fmt3(e.exposure[3] / e.exposureN);
        else
            line += ",expo=na";
        if (e.duckN)
            line += ",duck_flag=" + Fmt2(static_cast<double>(e.duckOn) / e.duckN) + ",duck_level=" +
                Fmt3(e.duckLevel / e.duckN) + ",duck_level_max=" + Fmt3(e.duckLevelMax) + ",duck_scene=" +
                (e.duckSceneN ? Hex(l.duckSceneHash) + ":" + Fmt3(e.duckSceneApply / e.duckSceneN)
                              : std::string("none")) +
                // The duck scene's weatherApply and sfxApply (the
                // level and flag above are reset every frame by 0x2F2D455:
                // the scene's variables are what the mixer applies).
                ",duck_weather=" + (e.duckWeatherN ? Fmt3(e.duckWeather / e.duckWeatherN) : std::string("none")) +
                ",duck_sfx=" + (e.duckSfxN ? Fmt3(e.duckSfx / e.duckSfxN) : std::string("none"));
        else
            line += ",duck_flag=na,duck_level=na,duck_level_max=na,duck_scene=na,duck_weather=na,duck_sfx=na";
        line += ",flags=" + (e.flagsRead ? FlagsText(e.flagsMax, flagHashes, kAudioFlagCount) : std::string("na"));
        line += ",music_mgr=" + std::to_string(l.music ? 1 : 0) + ",music_tracks=" + std::to_string(e.musicTracksMax);
        line += ",mix_scenes=" + std::to_string(scenes) + ",scenes_low=" + (lowScenes.empty() ? std::string("none") : lowScenes);
        return line;
    }

    // The mixer scenes below unity: "id:volume:mode:tag:key;..." (at most `top`).
    inline std::string LowScenesText(const std::vector<SceneInfo>& scenes, std::size_t top)
    {
        std::vector<const SceneInfo*> low;
        for (const SceneInfo& s : scenes)
            if (s.volume < 0.99f)
                low.push_back(&s);
        std::sort(low.begin(), low.end(), [](const SceneInfo* a, const SceneInfo* b) {
            return a->volume != b->volume ? a->volume < b->volume : a->id < b->id;
        });
        std::string out;
        for (std::size_t n = 0; n < low.size() && n < top; ++n)
        {
            char b[80];
            std::snprintf(b, sizeof(b), "%s%u:%.3f:%d:%d:0x%x", out.empty() ? "" : ";", low[n]->id, low[n]->volume,
                low[n]->mode, low[n]->tag, low[n]->key);
            out += b;
        }
        return out;
    }

    // ------------------------------------------------------------ pass totals

    // Power mean per class over the windows of a pass (VOLUME_PROBE_TOTAL).
    struct PassTotals
    {
        double power[kClsCount + 1] = {};
        double effPower[kClsCount + 1] = {};
        std::uint32_t windows = 0;
        void Add(const Window& w)
        {
            if (w.Empty())
                return;
            const double snaps = static_cast<double>(w.info.snaps);
            for (std::uint8_t n = 0; n < kClsCount; ++n)
            {
                power[n] += w.cls[n].power / snaps;
                effPower[n] += w.cls[n].effPower / snaps;
            }
            power[kClsCount] += w.all.power / snaps;
            effPower[kClsCount] += w.all.effPower / snaps;
            ++windows;
        }
        std::string Text(bool eff = false) const
        {
            std::string out;
            for (std::uint8_t n = 0; n <= kClsCount; ++n)
            {
                const char* name = n < kClsCount ? ClsName(static_cast<Cls>(n)) : "all";
                const double p = eff ? effPower[n] : power[n];
                out += std::string(out.empty() ? "" : "|") + name + ":" + Fmt1(PowerToDb(windows ? p / windows : 0.0));
            }
            return out;
        }
    };
}
