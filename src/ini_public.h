#pragma once

// The settings a player sees.
//
// kShippedIni is the RedemptionEditor.ini the mod writes on its first run (no
// RedemptionEditor.ini and no old ReplayProbe.ini in the game folder) and the
// file the release zip carries (dist/RedemptionEditor.ini, the same text;
// tools/check_public_build.py compares them). Every value in it is the code's
// default, so writing it changes nothing: a key left out reads the same value.
//
// Its parts:
//   [RedemptionEditor]  the replay buffer, the manual recording limits, clip
//                       compression, the world state, logging and reports;
//                       the editor's own preferences as commented lines
//                       (the editor writes them when they change in its menus)
//   [Export]            the video export
//   [Keys]              every action's keys (editor_keys.h kActions)
//
// The engine safety switches (kSupportSwitches) are not in it: they are all
// on, and support asks a player to add one by hand ("SoundGeneric=0" under
// [RedemptionEditor], or under [Troubleshooting]: mod_identity.h SectionFor
// reads both) when a crash report points at it. The public build still reads
// them. docs/release/troubleshooting-keys.md says what each one does and when
// to suggest it.
//
// kPublicReadKeys is every key the public build reads (a developer build,
// REDEMPTION_DEV, reads the others too). tools/check_public_build.py checks it
// against the sources; tests/ini_public_tests.cpp checks that each one is in
// kShippedIni (an active line or a commented ";Key=" line) or is a support
// switch, never both.

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace replay::inipublic
{
    inline constexpr char kShippedIni[] = R"INI(; Redemption Editor settings.
; Lines that start with ";" are comments. Every value below is the default:
; delete a line and the mod uses the same value. The mod reads this file when
; the game starts.

[RedemptionEditor]
; The language of the editor and its notices: auto (the game's language) or
; one of en, pt-BR, es, es-MX, fr, de, it, pl, ru, ja, ko, zh-CN, zh-TW (a file
; of the RedemptionEditor\lang folder, without .lang).
Language=auto

; The replay buffer: the mod always records the last seconds around you while
; nothing plays; the SaveBuffer key saves them as a clip. 1 on, 0 off.
ReplayBuffer=1
; How many seconds the buffer keeps (10 or more).
ReplayBufferSeconds=60
; The buffer's memory in MB (16 or more). In busy places (Saint Denis) the
; buffer may keep fewer seconds than asked; the save tells you when it does.
; The buffer's seconds and memory do not limit a manual recording.
; 16 to 960 (a saved buffer stays under the 1 GiB a clip file may hold).
ReplayBufferMB=128

; Manual recording (the Record key starts and stops it), with the buffer on or
; off. A long recording is saved in parts, each its own clip (names ending in
; -p1, -p2...): a part closes at ManualMaxMB (32 to 960; a busy town fills
; 900 MB in 2 to 3 minutes) or after ManualMaxSeconds (0: no time limit), a
; notice says so, and the recording goes on in the next part. With the buffer
; on, a part also holds up to ReplayBufferMB when that is larger.
ManualMaxSeconds=0
ManualMaxMB=900

; Clips are saved compressed. Clips you have not opened for CompressAfterDays
; days are compressed further in the background (they still open in the
; editor). Level 1 (fast) to 22 (smallest).
CompressClips=1
CompressAfterDays=1
CompressLevel=19

; Cutscenes: 0 pauses the recording while a cutscene plays, 1 records it.
; LockCutsceneCamera=1: camera markers inside a recorded cutscene keep the
; game's camera. Both are also in the editor's Replay Settings.
RecordCutscenes=0
LockCutsceneCamera=0
; 1: the Dead Eye look is played back with the clip (Replay Settings).
ReplayDeadEye=0
; 1: the world stays frozen while a replay is paused (Replay Settings).
FreezeWorldOnPause=1
; The REC notice while you record (Replay Settings): normal; brief (3 seconds
; when a recording starts, stops or saves); hidden (you see it, screen
; recorders such as OBS, NVIDIA and Game Bar do not; Windows 10 2004 or newer
; and the game windowed or borderless, else it works as brief); off.
RecordingIndicator=normal
; The size of the editor's menus, panels, texts and notices, in percent
; (Replay Settings): 80, 90, 100, 110 or 125. 100 looks the same at every
; resolution. A 4:3 screen stays at 100 at most.
InterfaceScale=100

; The map state of the clip (doors, buildings and other world pieces of the
; place where it was recorded) is put back while it plays. 0: only a notice.
RestoreWorldState=1
; 1: also while a mission runs (by default a mission's world is left alone).
RestoreWorldStateInMission=0

; Logging. 1 writes the full log (RedemptionEditor.log), what we need when
; you report a problem; 0 keeps only the essential lines (a smaller file).
FullLog=1

; Reports. The MarkProblem key marks a problem while a replay plays
; (QaMarks=1), the Report key saves a report ZIP in the ReplayReports folder
; (see the README).
QaMarks=1
; The report ZIP carries the clips you marked and no other.
; 1 also adds the clip open in the editor.
ReportIncludeClips=0

; The editor's preferences. The editor writes these when you change them in
; its menus (Free Camera, camera markers); they are shown here with their
; defaults.
;FreeCamMotion=classic
;FreeCamWalk=0
;FreeCamWalkHeight=1.7
;FreeCamLockAltitude=0
;FreeCamCollision=0
;FreeCamDroneDrag=3
;FreeCamDroneAcceleration=15
;FreeCamDroneGravity=0
;FreeCamDroneBanking=15
;FreeCamDroneRotSmoothing=8
;FreeCamDroneFovSmoothing=5
;FreeCamLookSensitivity=10
;FreeCamNearClip=0.1
;SplinePacing=natural
;SplineAlpha=1
;SplineEaseIn=0
;SplineEaseOut=0
;SplineWeight=0
;SplinePosition=1
;SplineRotation=1
;SplineFov=1
;CameraOverlay=1
;CameraOverlayWhilePlaying=0
;CameraOverlayTimeLabels=1
;CameraOverlayTrail=1

[Export]
; Video export (Ctrl+E in the clip editor). 1 on, 0 off. Needs the game on
; Vulkan; on DirectX 12 the export refuses to start.
Enabled=1
; Where videos go. Empty: Documents\Rockstar Games\Red Dead Redemption 2\Videos\Redemption Editor
Folder=
; The video encoder: auto, nvenc, amf, qsv or cpu.
Encoder=auto
; 1: HDR10 video.
Hdr10=0
; A .cube colour LUT applied to the export (a full path). Empty: none.
Lut=
; How many ms the game's sound is moved earlier in the video, so shots land on
; their muzzle flash.
AudioOffsetMs=140

[Keys]
; Every action's keys (the defaults below): one or two keys separated by a
; comma, with Shift+ or Ctrl+ in front (Shift+F4). Empty or None: no key. The
; editor's texts and its F1 key list show the keys set here.
PlayPause=K
FrameBack=Comma
FrameForward=Period
Rewind=
FastForward=
SpeedDown=LeftBracket
SpeedUp=RightBracket
FreeCam=F11
HideUi=H
Editor=F2
Stop=F7
Record=F4
SaveBuffer=Shift+F4
MarkProblem=Shift+F2
Report=Shift+F1
FreeCamDrone=V
FreeCamAcrobatic=N
FreeCamWalk=B
FreeCamCollision=Y
FreeCamLockAltitude=U
FreeCamFollow=F
FreeCamPick=G
FreeCamPickNearest=Shift+G
FreeCamLevel=Num5
FreeCamBehind=Num0
FreeCamLock=L
CameraOverlay=Backquote
)INI";

    struct ReadKey
    {
        const char* section; // "RedemptionEditor" (also [ReplayPoC], [Troubleshooting]), "Export" or "Keys"
        const char* key;
    };

    // Every key the public build reads (tools/check_public_build.py keeps this
    // list and the sources in step).
    inline constexpr ReadKey kPublicReadKeys[] = {
        // [RedemptionEditor]
        {"RedemptionEditor", "ReplayBuffer"}, {"RedemptionEditor", "ReplayBufferSeconds"}, {"RedemptionEditor", "ReplayBufferMB"},
        {"RedemptionEditor", "ManualMaxSeconds"}, {"RedemptionEditor", "ManualMaxMB"}, {"RedemptionEditor", "CompressClips"},
        {"RedemptionEditor", "CompressAfterDays"}, {"RedemptionEditor", "CompressLevel"}, {"RedemptionEditor", "RecordCutscenes"},
        {"RedemptionEditor", "LockCutsceneCamera"}, {"RedemptionEditor", "ReplayDeadEye"}, {"RedemptionEditor", "FreezeWorldOnPause"},
        {"RedemptionEditor", "RecordingIndicator"}, {"RedemptionEditor", "InterfaceScale"},
        {"RedemptionEditor", "RestoreWorldState"}, {"RedemptionEditor", "RestoreWorldStateInMission"}, {"RedemptionEditor", "FullLog"},
        {"RedemptionEditor", "QaMarks"}, {"RedemptionEditor", "ReportIncludeClips"}, {"RedemptionEditor", "Language"},
        {"RedemptionEditor", "FreeCamMotion"}, {"RedemptionEditor", "FreeCamWalk"},
        {"RedemptionEditor", "FreeCamWalkHeight"}, {"RedemptionEditor", "FreeCamLockAltitude"}, {"RedemptionEditor", "FreeCamCollision"},
        {"RedemptionEditor", "FreeCamDroneDrag"}, {"RedemptionEditor", "FreeCamDroneAcceleration"},
        {"RedemptionEditor", "FreeCamDroneGravity"}, {"RedemptionEditor", "FreeCamDroneBanking"},
        {"RedemptionEditor", "FreeCamDroneRotSmoothing"}, {"RedemptionEditor", "FreeCamDroneFovSmoothing"},
        {"RedemptionEditor", "FreeCamLookSensitivity"}, {"RedemptionEditor", "FreeCamNearClip"},
        {"RedemptionEditor", "SplinePacing"}, {"RedemptionEditor", "SplineAlpha"}, {"RedemptionEditor", "SplineEaseIn"},
        {"RedemptionEditor", "SplineEaseOut"}, {"RedemptionEditor", "SplineWeight"}, {"RedemptionEditor", "SplinePosition"},
        {"RedemptionEditor", "SplineRotation"}, {"RedemptionEditor", "SplineFov"},
        {"RedemptionEditor", "CameraOverlay"}, {"RedemptionEditor", "CameraOverlayWhilePlaying"},
        {"RedemptionEditor", "CameraOverlayTimeLabels"}, {"RedemptionEditor", "CameraOverlayTrail"},
        // The support switches (kSupportSwitches below; not in kShippedIni). Read through SectionFor:
        // [RedemptionEditor], [ReplayPoC], [Troubleshooting].
        {"RedemptionEditor", "WorldModel"}, {"RedemptionEditor", "SoundGeneric"}, {"RedemptionEditor", "TheatreHooks"}, {"RedemptionEditor", "TheatrePerschar"},
        {"RedemptionEditor", "TheatreVehicles"}, {"RedemptionEditor", "TheatrePeds"}, {"RedemptionEditor", "TheatrePauseScripts"},
        {"RedemptionEditor", "TheatreDenySpawn"}, {"RedemptionEditor", "TheatreEmitters"}, {"RedemptionEditor", "ReinsHoldGuard"},
        {"RedemptionEditor", "VehicleDoorWrite"}, {"RedemptionEditor", "WeaponWearWrite"}, {"RedemptionEditor", "WeaponPassPose"},
        {"RedemptionEditor", "WeaponFxIntent"}, {"RedemptionEditor", "DecalGate"}, {"RedemptionEditor", "RecordPedAnimVisible"},
        {"RedemptionEditor", "RecordPedAnimLod0"}, {"RedemptionEditor", "CategoryStateReplay"},
        {"RedemptionEditor", "SpeechReplayRules"}, {"RedemptionEditor", "SpeechDuckReplay"},
        {"RedemptionEditor", "SpeechConversationLive"}, {"RedemptionEditor", "PuppetOwnSpeech"},
        {"RedemptionEditor", "RefusePuppetOwnPersistent"}, {"RedemptionEditor", "ListenerMicReplay"},
        {"RedemptionEditor", "WindReplay"}, {"RedemptionEditor", "RenderTargetRecord"}, {"RedemptionEditor", "RenderTargetPlayback"},
        {"RedemptionEditor", "ObjectLook"}, {"RedemptionEditor", "MoonReplay"}, {"RedemptionEditor", "FxPuppetNoWarp"},
        {"RedemptionEditor", "FxPuppetVelocity"}, {"RedemptionEditor", "FxPauseFreeze"}, {"RedemptionEditor", "FxSettingsReplay"},
        {"RedemptionEditor", "FxRelocateClearStill"}, {"RedemptionEditor", "FootImpactReplay"}, {"RedemptionEditor", "FootLayerReplay"},
        {"RedemptionEditor", "FootWetnessReplay"}, {"RedemptionEditor", "PuppetAudioLodRecorded"},
        {"RedemptionEditor", "FootMovementReplay"}, {"RedemptionEditor", "MotionSpeedReplay"}, {"RedemptionEditor", "PedSpeedReplay"},
        {"RedemptionEditor", "FootEventReplay"}, {"RedemptionEditor", "EntityQueryRecord"}, {"RedemptionEditor", "EntityQueryReplay"},
        {"RedemptionEditor", "EntityQueryRelation"}, {"RedemptionEditor", "ShotEnvGroupReplay"}, {"RedemptionEditor", "FireCharCreate"},
        {"RedemptionEditor", "ObjectBurnLevel"}, {"RedemptionEditor", "PuppetScriptPose"}, {"RedemptionEditor", "FrameEnd"},
        {"RedemptionEditor", "PhysicsFilter"},
        // [Export]
        {"Export", "Enabled"}, {"Export", "Folder"}, {"Export", "Encoder"}, {"Export", "Hdr10"}, {"Export", "Lut"},
        {"Export", "AudioOffsetMs"},
        // [Keys]: every action of editor_keys.h kActions (tests/ini_public_tests.cpp checks the two lists match).
        {"Keys", "PlayPause"}, {"Keys", "FrameBack"}, {"Keys", "FrameForward"}, {"Keys", "Rewind"}, {"Keys", "FastForward"},
        {"Keys", "SpeedDown"}, {"Keys", "SpeedUp"}, {"Keys", "FreeCam"}, {"Keys", "HideUi"}, {"Keys", "Editor"}, {"Keys", "Stop"},
        {"Keys", "Record"}, {"Keys", "SaveBuffer"}, {"Keys", "MarkProblem"}, {"Keys", "Report"}, {"Keys", "FreeCamDrone"},
        {"Keys", "FreeCamAcrobatic"}, {"Keys", "FreeCamWalk"}, {"Keys", "FreeCamCollision"}, {"Keys", "FreeCamLockAltitude"},
        {"Keys", "FreeCamFollow"}, {"Keys", "FreeCamPick"}, {"Keys", "FreeCamPickNearest"}, {"Keys", "FreeCamLevel"},
        {"Keys", "FreeCamBehind"}, {"Keys", "FreeCamLock"}, {"Keys", "CameraOverlay"},
    };
    inline constexpr std::size_t kPublicReadKeyCount = sizeof(kPublicReadKeys) / sizeof(kPublicReadKeys[0]);

    // The engine safety switches: read by the public build (they are in
    // kPublicReadKeys), left out of kShippedIni. Each one turns on code that
    // changes or calls the game's engine (a detour, an engine function or a
    // memory write), and each one is on; the value is the code's default.
    // docs/release/troubleshooting-keys.md lists them for support.
    struct SupportSwitch
    {
        const char* key; // read from [RedemptionEditor] (or [ReplayPoC], [Troubleshooting])
        const char* value;
    };

    inline constexpr SupportSwitch kSupportSwitches[] = {
        {"WorldModel", "gta"}, {"SoundGeneric", "1"}, {"TheatreHooks", "31"}, {"TheatrePerschar", "1"},
        {"TheatreVehicles", "1"}, {"TheatrePeds", "1"}, {"TheatrePauseScripts", "1"}, {"TheatreDenySpawn", "1"},
        {"TheatreEmitters", "1"}, {"ReinsHoldGuard", "1"}, {"VehicleDoorWrite", "1"}, {"WeaponWearWrite", "1"},
        {"WeaponPassPose", "1"}, {"WeaponFxIntent", "1"}, {"DecalGate", "1"}, {"RecordPedAnimVisible", "1"},
        {"RecordPedAnimLod0", "1"}, {"CategoryStateReplay", "1"}, {"SpeechReplayRules", "1"}, {"SpeechDuckReplay", "1"},
        {"SpeechConversationLive", "1"}, {"PuppetOwnSpeech", "1"}, {"RefusePuppetOwnPersistent", "1"},
        {"ListenerMicReplay", "1"}, {"WindReplay", "1"}, {"RenderTargetRecord", "1"}, {"RenderTargetPlayback", "1"},
        {"ObjectLook", "1"}, {"MoonReplay", "1"}, {"FxPuppetNoWarp", "1"}, {"FxPuppetVelocity", "1"},
        {"FxPauseFreeze", "1"}, {"FxSettingsReplay", "1"}, {"FxRelocateClearStill", "1"}, {"FootImpactReplay", "1"},
        {"FootLayerReplay", "1"}, {"FootWetnessReplay", "1"}, {"PuppetAudioLodRecorded", "1"},
        {"FootMovementReplay", "1"}, {"MotionSpeedReplay", "1"}, {"PedSpeedReplay", "1"}, {"FootEventReplay", "1"},
        {"EntityQueryRecord", "1"}, {"EntityQueryReplay", "1"}, {"EntityQueryRelation", "1"},
        {"ShotEnvGroupReplay", "1"}, {"FireCharCreate", "1"}, {"ObjectBurnLevel", "1"}, {"PuppetScriptPose", "1"},
        {"FrameEnd", "1"}, {"PhysicsFilter", "1"},
    };
    inline constexpr std::size_t kSupportSwitchCount = sizeof(kSupportSwitches) / sizeof(kSupportSwitches[0]);

    // The support switch named `key` (nullptr: not one).
    inline const SupportSwitch* FindSupportSwitch(const char* key)
    {
        for (const SupportSwitch& entry : kSupportSwitches)
            if (std::strcmp(entry.key, key) == 0)
                return &entry;
        return nullptr;
    }

    // One line of kShippedIni that names a key.
    struct IniLine
    {
        std::string section;
        std::string key;
        std::string value;
        bool commented = false; // ";Key=value": documented, not set
    };

    namespace detail
    {
        inline bool KeyChar(char c)
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        }
    }

    // The keys of an ini text in order: "Key=value" lines and ";Key=value"
    // lines (a ';' then the key at once; a comment has a space after ';').
    inline std::vector<IniLine> Parse(const char* text)
    {
        std::vector<IniLine> out;
        std::string section;
        const char* at = text;
        while (at && *at)
        {
            const char* end = std::strchr(at, '\n');
            std::string line = end ? std::string(at, end) : std::string(at);
            at = end ? end + 1 : nullptr;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (line.empty())
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            bool commented = false;
            std::size_t begin = 0;
            if (line.front() == ';')
            {
                commented = true;
                begin = 1;
            }
            std::size_t n = begin;
            while (n < line.size() && detail::KeyChar(line[n]))
                ++n;
            if (n == begin || n >= line.size() || line[n] != '=')
                continue; // a comment or an empty ';'
            IniLine entry;
            entry.section = section;
            entry.key = line.substr(begin, n - begin);
            entry.value = line.substr(n + 1);
            entry.commented = commented;
            out.push_back(entry);
        }
        return out;
    }

    // The [RedemptionEditor] keys may sit in [RedemptionEditor] or
    // [Troubleshooting] (SectionFor reads both; the shipped ini has no
    // [Troubleshooting] any more, a hand-written one is still read).
    inline bool SameSection(const std::string& lineSection, const char* readSection)
    {
        if (lineSection == readSection)
            return true;
        return std::strcmp(readSection, "RedemptionEditor") == 0 && lineSection == "Troubleshooting";
    }

    // The shipped line of a key read from `section` (nullptr: not documented).
    inline const IniLine* Find(const std::vector<IniLine>& lines, const char* section, const char* key)
    {
        for (const IniLine& line : lines)
            if (line.key == key && SameSection(line.section, section))
                return &line;
        return nullptr;
    }
}
