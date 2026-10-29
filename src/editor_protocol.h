#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <type_traits>

namespace replay::editor
{
    // Shared memory protocol between the core and the editor add-on. Both
    // versions are raised whenever the Command or Snapshot layout changes.
    constexpr std::uint32_t kApiVersion = 26;
    constexpr std::uint32_t kMaxCameraKeyframesInSnapshot = 512; // a directed clip has hundreds
    constexpr std::uint32_t kSharedTransportMagic = 0x42504552u; // REPB
    constexpr std::uint32_t kSharedTransportVersion = 25;
    constexpr std::uint32_t kMaxClipsInSnapshot = 64;
    constexpr std::uint32_t kMaxTargetsInSnapshot = 32;
    constexpr std::uint32_t kTargetLabelCapacity = 24;
    constexpr std::size_t kClipNameCapacity = 64;
    constexpr std::size_t kSharedCommandCapacity = 256;
    constexpr std::size_t kSharedTransportNameCapacity = 96;
    // Restricted-camera spans and cuts of the clip in memory.
    constexpr std::uint32_t kMaxSpansInSnapshot = 64;
    constexpr std::uint32_t kMaxCutsInSnapshot = 64;
    // The open project's clips (all of them: the GTA V limit on PC,
    // replay_project.h kMaxClips) and the project files of ReplayProjects
    // (newest first).
    constexpr std::uint32_t kMaxProjectClipsInSnapshot = 512;
    constexpr std::uint32_t kMaxProjectsInSnapshot = 64;
    constexpr std::size_t kProjectNameCapacity = 64;

    enum SnapshotFlags : std::uint32_t
    {
        SnapshotCoreReady = 1u << 0,
        SnapshotRecording = 1u << 1,
        SnapshotPlaying = 1u << 2,
        SnapshotPaused = 1u << 3,
        SnapshotFreeCamEnabled = 1u << 4,
        SnapshotCameraTrackEnabled = 1u << 5,
        SnapshotCameraTrackManualOverride = 1u << 6,
        SnapshotCommandQueueOverflowed = 1u << 7,
        SnapshotScrubbing = 1u << 8, // playhead moved by a scrub key
        SnapshotClipFromDisk = 1u << 9, // the clip in memory came from a file
        SnapshotPreparing = 1u << 10,    // playback holds while the first puppets are built
        SnapshotDebugMode = 1u << 11,    // RedemptionEditor.ini Debug=1: the text HUD, no playback bar
        SnapshotBufferRunning = 1u << 12, // the always-on replay buffer is recording
        SnapshotBufferManual = 1u << 13,  // F4 marked a manual recording inside the buffer
        SnapshotGamePaused = 1u << 14,    // the game's pause menu is open (no thumbnail frames)
        SnapshotSavingClip = 1u << 15,    // a clip file is being written (the "saving" notice)
        SnapshotEditingCamera = 1u << 16, // "Edit Camera": the free camera frames marker editingMarker
        SnapshotReplayDeadEye = 1u << 17, // recorded Dead Eye plays in the replay (Replay Settings)
        SnapshotRecordCutscenes = 1u << 18,    // RedemptionEditor.ini RecordCutscenes=1 (cutscenes kept in the clip)
        SnapshotLockCutsceneCamera = 1u << 19, // LockCutsceneCamera=1 (markers in a cutscene are the game camera)
        SnapshotClipEdited = 1u << 20,         // the clip's markers changed since it was loaded or saved
        SnapshotMarkerCopied = 1u << 21,       // a marker's settings are in the clipboard (Paste works)
        SnapshotLoadFailed = 1u << 22,         // the last clip load failed (set for 5 s)
        SnapshotCanUndo = 1u << 23,            // a marker change can be undone (Ctrl+Z)
        SnapshotCanRedo = 1u << 24,            // an undone change can be redone (Ctrl+Y)
        SnapshotFreezeOnPause = 1u << 25,      // FreezeWorldOnPause=1 (the paused replay freezes the world)
        SnapshotRenameFailed = 1u << 26,       // the last clip rename failed (set for 5 s; the name is taken or the file is in use)
        SnapshotClipListLoading = 1u << 27,    // the clips folder is being read (on a thread); the list shown is the last one
        SnapshotOpeningClip = 1u << 28,        // a gallery load waits for the save of the clip left
        SnapshotSaveFailed = 1u << 29,         // a clip save failed; its data is held in memory and retried
        SnapshotExporting = 1u << 30,          // render export sketch: an export runs (progress in the export channel)
        // The world loads behind the black: a clip's load wait (playing) or the
        // wait after a clip closes (not playing). The editor draws the loading
        // cue (the turning cylinder and its caption) while it is set.
        SnapshotLoadingWorld = 1u << 31
    };

    enum class Easing : std::uint32_t
    {
        Linear = 0,
        SmoothStep = 1,
        EaseIn = 2,
        EaseOut = 3,
        Hold = 4
    };

    enum class Path : std::uint32_t
    {
        Linear = 0,
        Smooth = 1
    };

    // What a camera keyframe shows, after the Rockstar Editor's
    // markers: its own pose, the recorded game camera, a fixed position
    // looking at a target entity, or a position that follows the target at
    // a fixed offset (in the target's heading frame) looking at it.
    //
    // API 8: the Rockstar Editor's marker camera types. Target and Follow are kept
    // only to read CAMR v2 clips and for the old core view; a marker built
    // by the new rules is Free (with Look At and Attach), Game or a preset.
    enum class CameraMode : std::uint32_t
    {
        Free = 0,
        Game = 1,
        Target = 2,   // legacy: Free + Look At
        Follow = 3,   // legacy: Free + Attach Default + Look At on the same target
        FrontLow = 4,
        Rear = 5,
        RightSide = 6,
        LeftSide = 7,
        Overhead = 8
    };

    // How the camera goes from this marker to the next one (the blend lives
    // on the source marker). None is a cut at the next marker.
    enum class Blend : std::uint8_t
    {
        None = 0,
        Linear = 1,
        Smooth = 2,
        // A curve through the markers (time-aware Hermite, Simple Camera's
        // path): the move keeps its speed through each marker instead of
        // turning a corner.
        // A run of Free markers blending into each other is one curve.
        Spline = 3
    };
    constexpr std::uint8_t kBlendCount = 4;

    // How a Spline segment plays. The ideas come from the
    // REPlus mod's spline (crxhvrd; read for the maths only, our own code).
    //  - pacing: Natural (the curve over the marker times),
    //    Continuous (one speed curve through the whole move: arc length and a
    //    monotone cubic of time to distance, markers hit on time), Per Segment
    //    (arc length per segment).
    //  - alpha: the Catmull-Rom knot spacing (0 Uniform, 0.5 Centripetal,
    //    1 Chordal; any value between). Only off Natural (a curve over the
    //    marker times has no knot spacing to choose).
    //  - easeIn, easeOut: the fraction of the segment spent speeding up and
    //    slowing down (a trapezoid of speed that still arrives on time); it
    //    remaps time, so every channel eases together.
    //  - weight: 0..1, the curve pulled toward a cubic B-spline over the same
    //    markers (it cuts inside the corners like a camera with mass). Only
    //    while the clip plays or exports.
    //  - channels: which channels the curve drives (the others go straight).
    // A marker's tuning has `overrides`: the fields it sets itself; the
    // others come from the global defaults (RedemptionEditor.ini Spline*).
    enum class SplinePacing : std::uint8_t
    {
        Natural = 0,
        Continuous = 1,
        PerSegment = 2
    };
    constexpr std::uint8_t kSplinePacingCount = 3;

    enum SplineChannelBits : std::uint8_t
    {
        SplineChannelPosition = 1u << 0,
        SplineChannelRotation = 1u << 1,
        SplineChannelFov = 1u << 2,
        SplineChannelAll = 7
    };

    enum SplineOverrideBits : std::uint8_t
    {
        SplineOverrideAlpha = 1u << 0,
        SplineOverrideEaseIn = 1u << 1,
        SplineOverrideEaseOut = 1u << 2,
        SplineOverrideWeight = 1u << 3,
        SplineOverridePacing = 1u << 4,
        SplineOverridePosition = 1u << 5,
        SplineOverrideRotation = 1u << 6,
        SplineOverrideFov = 1u << 7
    };

    struct SplineTuning
    {
        float alpha;          // 0..1
        float easeIn;         // 0..1 of the segment
        float easeOut;        // 0..1 of the segment
        float weight;         // 0..1
        std::uint8_t pacing;  // SplinePacing
        std::uint8_t channels; // SplineChannelBits
        std::uint8_t overrides; // SplineOverrideBits (a marker's; ignored in the defaults)
        std::uint8_t reserved;
    };

    // The defaults: Natural, Chordal (REPlus's choice; it only matters off
    // Natural), no ease, no weight, every channel. A marker starts with no
    // override (everything from the defaults).
    inline SplineTuning DefaultSplineTuning()
    {
        return SplineTuning{1.0f, 0.0f, 0.0f, 0.0f, static_cast<std::uint8_t>(SplinePacing::Natural), SplineChannelAll, 0, 0};
    }

    // The marker's shake follows the camera's
    // speed (Simple Camera's speed coupling): stronger on a fast move, and
    // with SpeedStill gone while the camera stands still.
    enum class ShakeMotion : std::uint8_t
    {
        Off = 0,
        Speed = 1,
        SpeedStill = 2
    };
    constexpr std::uint8_t kShakeMotionCount = 3;

    // Which part of the attach target's rotation the camera follows:
    // Rigid all of it, Default the heading only, Flexible none (position only).
    enum class AttachType : std::uint8_t
    {
        Rigid = 0,
        Default = 1,
        Flexible = 2
    };

    enum class ShakeType : std::uint8_t
    {
        None = 0,
        Handheld = 1,
        Drunk = 2,
        GroundVibration = 3,
        AirTurbulence = 4,
        Explosion = 5,
        // Added after the Rockstar Editor's set (realistic shake, camera_shake.h).
        Walking = 6,
        Running = 7,
        Horseback = 8,
        Steadicam = 9,
        Wagon = 10,
        // Simple Camera's numbers (ShakeCustom) instead of a profile.
        Custom = 11,
        // A short recoil kick (one shot, about 0.3 s), after Custom so the
        // older ids keep their values.
        Gunshot = 12
    };
    constexpr std::uint8_t kShakeTypeCount = 13;

    // The numbers of a Custom shake (Simple Camera's
    // Camera Effects, crxhvrd, MIT): a 6 channel noise of 3 octaves of sines
    // with incommensurate per axis frequencies. Rotation reaches amp x 1
    // degree x rotWeight, position amp x 5 cm x posWeight; the camera's speed
    // (from the marker curve, normalised by speedRef and held to 1) raises
    // the amplitude (1 + speedAmp x speed) and the frequency (1 + speedHz x
    // speed); `still` fades it out while the camera stands still; `seed`
    // picks the pattern (0: Simple Camera's own table). The marker's or the
    // event's intensity scales it as a gain (5 = the numbers as they are).
    struct ShakeCustom
    {
        float amp;        // 0..5
        float hz;         // 0.1..30
        float speedAmp;   // 0..2
        float speedHz;    // 0..2
        float rotWeight;  // 0..2
        float posWeight;  // 0..2
        float speedRef;   // m/s, 1..100: the speed counted as full
        std::uint16_t seed;
        std::uint8_t still; // 0/1
        std::uint8_t reserved;
    };

    // Simple Camera's Handheld preset (the numbers a new Custom starts from).
    inline ShakeCustom DefaultShakeCustom()
    {
        return ShakeCustom{0.4f, 4.0f, 1.0f, 0.5f, 1.0f, 1.0f, 30.0f, 0, 0, 0};
    }

    // The marker owns its shake, as a Rockstar Editor marker does (its shake
    // type, intensity and speed per marker, from that marker on); the old
    // separate shake events track (Simple Camera's model) is folded into the
    // markers on load. The marker holds the whole state:
    // on/off, a realistic profile or Simple Camera's noise (profile 0, with
    // its numbers), the intensity's gain, amp and frequency, the advanced
    // numbers, the pattern seed, the clock speed, and the transition to the
    // next shake marker (Cut, or Blend: the numbers named by blendMask go
    // linearly to the next marker's). Pure and deterministic
    // (camera_marker_shake.h): a scrub shows what the play shows.
    enum class ShakeTransition : std::uint8_t
    {
        Cut = 0,   // the state holds until the next marker (Simple Camera's Snap)
        Blend = 1  // the numbers in blendMask go to the next marker's (Ramp)
    };
    constexpr std::uint8_t kShakeTransitionCount = 2;
    enum ShakeFlagBits : std::uint8_t
    {
        ShakeFlagRestart = 1u << 0,         // the pattern and the fade in start again here (from startCycles, startFadeMs)
        ShakeFlagNewPattern = 1u << 1,      // only the pattern's phase starts again (Simple Camera's Randomize)
        ShakeFlagFadeOut = 1u << 2,         // Cut: fades out over the last 250 ms before the next marker
        ShakeFlagEaseWithCamera = 1u << 3,  // Blend: the gain follows the camera blend's ease
        ShakeFlagRealTime = 1u << 4         // the clock runs at rate / this marker's playback speed
    };
    enum ShakeBlendBits : std::uint16_t
    {
        ShakeBlendGain = 1u << 0,
        ShakeBlendAmp = 1u << 1,
        ShakeBlendHz = 1u << 2,
        ShakeBlendSpeedAmp = 1u << 3,
        ShakeBlendSpeedHz = 1u << 4,
        ShakeBlendRot = 1u << 5,
        ShakeBlendPos = 1u << 6,
        ShakeBlendSpeedRef = 1u << 7,
        ShakeBlendAll = 0xFFu
    };
    struct MarkerShake
    {
        std::uint8_t on;          // 0/1
        std::uint8_t profile;     // ShakeType 0..10 or 12 (Gunshot); 0: Simple Camera's noise with the numbers
        std::uint8_t transition;  // ShakeTransition, to the next shake marker
        std::uint8_t flags;       // ShakeFlagBits
        std::uint16_t blendMask;  // ShakeBlendBits (Blend)
        std::uint16_t seed;       // the noise's pattern (0: Simple Camera's own table)
        std::uint8_t still;       // 0/1: Stop When Still
        std::uint8_t reserved[3];
        float gain;               // the intensity's gain ((i + 1) / 6)^2: 1 is intensity 5
        float amp;                // 0..5 (a profile: 1 is the profile as tuned)
        float hz;                 // 0..30 (a profile: 4 Hz plays it at its own speed)
        float speedAmp;           // 0..2: the camera's speed raises the amplitude
        float speedHz;            // 0..2: the camera's speed raises the frequency
        float rotWeight;          // 0..2
        float posWeight;          // 0..2
        float speedRef;           // 1..100 m/s: the speed counted as full
        float rate;               // the clock's speed (shake speed), 1 = 100 %
        double startCycles;       // Restart: the pattern's phase there (cycles)
        double startFadeMs;       // Restart: the fade in already run there (ms)
    };
    constexpr float kShakeRateMin = 0.01f;
    constexpr float kShakeRateMax = 40.0f;
    // Off, Simple Camera's Handheld numbers, intensity 5, clock 100 %.
    inline MarkerShake DefaultMarkerShake()
    {
        MarkerShake s{};
        s.gain = 1.0f;
        s.amp = 0.4f;
        s.hz = 4.0f;
        s.speedAmp = 1.0f;
        s.speedHz = 0.5f;
        s.rotWeight = 1.0f;
        s.posWeight = 1.0f;
        s.speedRef = 30.0f;
        s.rate = 1.0f;
        s.blendMask = ShakeBlendAll;
        return s;
    }

    enum class DofMode : std::uint8_t
    {
        Default = 0,
        Custom = 1,
        None = 2
    };

    enum class FocusMode : std::uint8_t
    {
        Auto = 0,
        Manual = 1,
        Target = 2
    };

    // Anchor Off, Anchor On, Exclusive: an Anchor (exclusive) marker has no
    // camera options and the evaluator skips it.
    enum class MarkerType : std::uint8_t
    {
        Edit = 0,
        EditAndAnchor = 1,
        Anchor = 2
    };

    // What PasteCameraKeyframe copies. Only All carries anything today;
    // effects and audio per marker do not exist yet.
    enum class CopyContext : std::uint32_t
    {
        All = 0,
        EffectsOnly = 1,
        AudioOnly = 2
    };

    enum class CommandType : std::uint32_t
    {
        None = 0,
        PlayPause = 1,
        Stop = 2,
        SeekAbsolute = 3,     // timeMs, playback paused there
        StepFrame = 4,        // direction -1 or +1, one 30 Hz frame, paused
        ToggleFreeCam = 5,
        CaptureCameraKeyframe = 6,
        DeleteCameraKeyframe = 7,
        MoveCameraKeyframe = 8,
        UpdateCameraKeyframe = 9,
        SetCameraTrackEnabled = 10,
        ToggleRecording = 11,
        ReservedSetPlaybackDirection = 12,
        SetPlaybackSpeed = 13, // enabled = index in the speed presets
        LoadClip = 14,         // name: file name in ReplayClips
        DeleteClip = 15,       // name
        RefreshClips = 16,     // timeMs: the window's first clip, enabled: sort (ClipInfo::listIndex)
        SetEditorOpen = 17,    // enabled: the editor covers the screen, the core hides its text HUD
        SetCameraKeyframeMode = 18, // keyframeId, mode, target (packed replay id)
        SaveBuffer = 19,            // the replay buffer's window to a clip (Shift+F4)
        SetReplayBuffer = 20,       // enabled: on/off, timeMs: window seconds, keyframeId: memory MB (saved to the ini)
        // API 8, markers in the Rockstar Editor's style (keyframeId names the marker):
        SetCameraKeyframeOptions = 21, // options: the fields named by options.mask
        ResetCameraKeyframe = 22,      // back to the defaults, keeping the time
        CopyCameraKeyframe = 23,       // to the core's clipboard
        PasteCameraKeyframe = 24,      // enabled: CopyContext
        EditCameraKeyframe = 25,       // enabled: 1 enters the marker's Edit Camera mode, 0 leaves it
        SetReplayDeadEye = 26,         // enabled: the recorded Dead Eye plays in the replay (saved to the ini)
        SetClipCompression = 27,       // enabled: automatic compression on/off, keyframeId: days (saved to the ini)
        CompressClipsNow = 28,         // compress the clips older than the days now
        SetRecordCutscenes = 29,       // enabled: cutscenes are recorded (saved to the ini)
        SetLockCutsceneCamera = 30,    // enabled: markers in a recorded cutscene use the game camera (saved to the ini)
        // A menu sound (enabled: UiSound, timeMs: the editor's
        // GetTickCount64 when asked; the core drops it when it comes late).
        PlayUiSound = 31,
        CompressClip = 32, // name: that clip compressed now, whatever its age (the gallery's C)
        SaveOpenClip = 33, // the clip in memory and its markers to its file (a new one for an unsaved buffer clip)
        UndoMarkers = 34,  // the markers as before the last change (Ctrl+Z)
        RedoMarkers = 35,  // the change undone again (Ctrl+Y)
        RenameClip = 36,   // name: "old|new", file names in ReplayClips (the thumbnail goes along)
        SetFreezeOnPause = 37, // enabled: the paused replay freezes the world (saved to the ini)
        // Projects (a montage of clips, as GTA V's Rockstar Editor has).
        // A project index goes in keyframeId; a refused edit says why in
        // Snapshot::projectMessage.
        ProjectNew = 38,          // name: the project's name (empty: "Project N"); the open one is saved and closed
        ProjectOpen = 39,         // name: a file of ReplayProjects ("Chase.rpproj")
        ProjectSave = 40,
        ProjectClose = 41,        // saved first when changed (never lost, as a clip's markers)
        ProjectRename = 42,       // name: "file|new name"
        ProjectDelete = 43,       // name: a file of ReplayProjects
        RefreshProjects = 44,     // the project list read again
        ProjectAddClip = 45,      // name: a clip file; keyframeId: where (~0u: the end)
        ProjectMoveClip = 46,     // keyframeId: the clip; target: its new place
        ProjectDuplicateClip = 47, // keyframeId: a copy right after it
        ProjectDeleteClip = 48,   // keyframeId
        ProjectSplitClip = 49,    // keyframeId: the clip open at the playhead, cut in two there
        ProjectTrimClip = 50,     // keyframeId: the clip open; direction -1: its start to the playhead, +1: its end
        ProjectEditClip = 51,     // keyframeId: open it in the clip editor (its trim and markers)
        ProjectSeek = 52,         // timeMs: project time; enabled: kSeekPlain, kSeekDragging, kSeekDragEnd
        ProjectPlay = 53,         // enabled 1: play the project from timeMs (~0ull: from where it is), 0: pause

        // Render export. The settings travel in the export channel (src/export_channel.h), not in the Command, so the
        // Command layout does not change. Refused unless RedemptionEditor.ini
        // [Export] Enabled=1.
        StartExport = 54,  // the export channel's settings, requestSeq in keyframeId
        CancelExport = 55, // stops the export clock and gives the game timer back

        // One environment setting of the clip in playback steps (keyframeId: EnvironmentField, direction:
        // -1 or +1; Recorded is each one's first value). The core keeps the
        // value (env_override.h) and shows it in Snapshot::env*.
        SetEnvironment = 56,

        // Marker tools and the free camera (Simple Camera's set). A "move" is the run of
        // markers that blend into each other around keyframeId.
        DuplicateCameraKeyframe = 57,   // keyframeId: a copy halfway to the next marker (2 s on when it is the last)
        DistributeCameraKeyframes = 58, // keyframeId: a marker of the move; enabled 0: even times, 1: constant speed (by distance)
        StretchCameraKeyframes = 59,    // keyframeId: a marker of the move; enabled: its new length in percent (25..400)
        LockCameraKeyframes = 60,       // keyframeId; target: packed id (0: back to world poses); enabled 0: the move, 1: every marker; options.attachType
        SetFreeCamOption = 61,          // keyframeId: FreeCamField; camera.fov: the value
        FreeCamAction = 62,             // keyframeId: FreeCamActionId
        SetCameraOverlay = 63,          // keyframeId: OverlayField; enabled: on/off
        // 64..67: the retired shake events track's commands (the shake is in
        // the markers). The numbers stay reserved.
        RetiredShakeEvent64 = 64,
        RetiredShakeEvent65 = 65,
        RetiredShakeEvent66 = 66,
        RetiredShakeEvent67 = 67,
        // name: a short line the core writes to its log as CAMERA_FX_UI (why the shake event's menu opened or closed).
        EditorUiNote = 68,
        // The Spline defaults (Command::spline; overrides ignored),
        // written back to RedemptionEditor.ini.
        SetSplineDefaults = 69,
        // enabled: the recording indicator's mode (rec_indicator.h Mode; saved to the ini).
        SetRecordingIndicator = 70,
        // enabled 1: the editor draws the loading cue while the world loads
        // (SnapshotLoadingWorld), so the core leaves out its own "Loading
        // replay..." text. Sent every 2 s; the core keeps it for 5 s, so an
        // add-on that stopped drawing gets the core's text back.
        SetLoadingCue = 71
    };


    // SetFreeCamOption's field (Snapshot::freeCamValues index).
    enum class FreeCamField : std::uint32_t
    {
        Motion = 0,         // 0 Classic, 1 Drone
        Acrobatic = 1,      // 0/1
        Walk = 2,           // 0/1
        WalkHeight = 3,     // metres
        LockAltitude = 4,   // 0/1
        Collision = 5,      // 0/1
        Follow = 6,         // freecam::Follow
        DroneDrag = 7,
        DroneAcceleration = 8,
        DroneGravity = 9,
        DroneBanking = 10,
        DroneRotSmoothing = 11,
        DroneFovSmoothing = 12,
        LookSensitivity = 13,
        ResetDefaults = 14, // every field back to its default (value ignored)
        NearClip = 15,      // metres, 0.01..1 (the flying free camera)
        Count = 16
    };

    enum class FreeCamActionId : std::uint32_t
    {
        PickAimed = 0,      // the target under the crosshair
        PickNearest = 1,    // the nearest target
        ClearTarget = 2,
        CycleFollow = 3,    // Off, Rigid, Translate
        LevelHorizon = 4,
        ShotBehindMain = 5, // the camera behind the main character (or its target)
        ToggleLock = 6,     // the camera holds still (input ignored)
        SetTarget = 7       // command.target: that packed id (0: none)
    };

    enum class OverlayField : std::uint32_t
    {
        Enabled = 0,      // markers, path and labels in the world
        WhilePlaying = 1, // also (dimmed) while the clip plays
        TimeLabels = 2,
        TargetTrail = 3   // the recorded path of the free camera's target
    };

    enum FreeCamFlags : std::uint32_t
    {
        FreeCamAcrobatic = 1u << 0,
        FreeCamWalk = 1u << 1,
        FreeCamLockAltitude = 1u << 2,
        FreeCamCollision = 1u << 3,
        FreeCamLocked = 1u << 4,
        FreeCamPad = 1u << 5,        // the pad moved the camera last
        FreeCamTargetLost = 1u << 6  // the target is not in the clip at the playhead
    };

    enum OverlayFlags : std::uint32_t
    {
        OverlayEnabled = 1u << 0,
        OverlayWhilePlaying = 1u << 1,
        OverlayTimeLabels = 1u << 2,
        OverlayTargetTrail = 1u << 3
    };

    // SetEnvironment's keyframeId.
    enum class EnvironmentField : std::uint32_t
    {
        Clock = 0,
        Weather = 1,
        Blend = 2,
        Clouds = 3,
        ResetAll = 4 // every setting back to Recorded
        // 5..10 were the experimental sun and moon rows (cut from 1.0).
    };

    // Snapshot::envFlags.
    enum EnvironmentFlags : std::uint32_t
    {
        EnvironmentRecorded = 1u << 0, // the clip has its clock and weather
        EnvironmentSkyTrack = 1u << 1, // the clip has the cloud track (SKYT)
        EnvironmentEditable = 1u << 2  // a clip plays and its settings can change
    };

    // Snapshot::projectFlags.
    enum ProjectFlags : std::uint32_t
    {
        ProjectOpenFlag = 1u << 0,      // a project is open
        ProjectDirty = 1u << 1,         // changed since it was opened or saved
        ProjectPreviewing = 1u << 2,    // the project plays as a whole (clips one after the other)
        ProjectEditingClip = 1u << 3,   // one of its clips is open in the clip editor
        ProjectLoadingClip = 1u << 4,   // the next clip is being read or prepared
        ProjectSaveFailed = 1u << 5,    // the last save failed (set for 5 s)
        ProjectListLoading = 1u << 6,   // reserved: the list is read at once today
        ProjectClipPreloaded = 1u << 7  // the next clip's file is already in memory
    };

    // ProjectClipInfo::flags.
    enum ProjectClipFlags : std::uint32_t
    {
        ProjectClipMissing = 1u << 0,    // the clip file is not in ReplayClips any more
        ProjectClipOwnMarkers = 1u << 1, // it was opened: its markers live in the project
        ProjectClipActive = 1u << 2      // the clip in playback now
    };

    // A clip of the open project, in order.
    struct ProjectClipInfo
    {
        char file[kClipNameCapacity];
        std::uint32_t inMs;
        std::uint32_t outMs;
        std::uint32_t rawMs;
        std::uint32_t flags;
        std::uint32_t markerCount;
        std::uint32_t reserved;
    };

    // A project file of ReplayProjects.
    struct ProjectInfo
    {
        char file[kClipNameCapacity];
        char name[kProjectNameCapacity];
        std::uint64_t totalMs;
        std::uint64_t modified; // FILETIME of the last write
        std::uint32_t clipCount;
        std::uint32_t readable;
    };

    // The editor's menu sounds, the game's shop menus' set
    // (HUD_SHOP_SOUNDSET, used by the shop_*.c scripts for the same moves).
    enum class UiSound : std::uint32_t
    {
        NavUp = 0,
        NavDown = 1,
        NavLeft = 2,
        NavRight = 3,
        Select = 4,
        Back = 5,
        Error = 6,
        Count = 7
    };

    inline const char* UiSoundName(UiSound sound)
    {
        static const char* const kNames[] = {"NAV_UP", "NAV_DOWN", "NAV_LEFT", "NAV_RIGHT", "SELECT", "BACK", "NAV_ERROR"};
        const std::uint32_t index = static_cast<std::uint32_t>(sound);
        return index < static_cast<std::uint32_t>(UiSound::Count) ? kNames[index] : nullptr;
    }

    constexpr const char* kUiSoundSet = "HUD_SHOP_SOUNDSET";

    struct ClipInfo
    {
        char name[kClipNameCapacity]; // file name without the folder
        std::uint64_t durationMs;     // 0 when the header is unreadable
        std::uint64_t bytes;
        std::uint32_t formatVersion;  // the file's; readable only when it equals this build's
        std::uint32_t loadable;
        // From the clip's environment and place sections; empty
        // text when the clip does not have them.
        std::uint32_t hasClock;
        std::uint32_t clockStartS;    // seconds since midnight
        char weather[24];
        char region[64];              // "town, district, state" as the game names them
        char character[16];
        // A compressed clip (clip_compress.h); `bytes` is the
        // size on disk, clipBytes the clip's own size (0 when unknown).
        std::uint64_t clipBytes;
        std::uint32_t compressed;
        // The clip's world info section (world_notice.h): the
        // chapter it was recorded in (0 unknown) and the camp (255 unknown).
        std::uint32_t chapter;
        std::uint32_t camp;
        // This clip's place in the whole sorted list (the
        // snapshot carries a window of kMaxClipsInSnapshot of them, moved by
        // RefreshClips: timeMs the window's first index, enabled the sort,
        // 0 newest first, 1 longest first).
        std::uint32_t listIndex;
    };

    // A restricted-camera span of the clip (record_gate.h
    // SpanKind: 0 cutscene, 1 script camera, 2 no control).
    struct ClipSpan
    {
        std::uint32_t startMs;
        std::uint32_t endMs;
        std::uint32_t kind;
        std::uint32_t reserved;
    };

    struct Vec3
    {
        float x;
        float y;
        float z;
    };

    struct Vec2
    {
        float x;
        float y;
    };

    struct CameraState
    {
        Vec3 position;
        Vec3 rotation;
        float fov;
    };

    struct CameraKeyframe
    {
        std::uint32_t id;
        std::uint32_t reserved;
        std::uint64_t timeMs;
        CameraState camera;
        // The old core view (API 7): what the current editor panel shows.
        // Kept until the editor moves to markers; camera::FillSnapshotKeyframe
        // fills them from the marker.
        Easing easing;
        Path path;
        CameraMode mode;      // legacy mode: Free, Game, Target, Follow
        std::uint32_t target; // packed replay id, 0 = none
        // API 8: the marker's visible options (everything but the relative
        // attach pose and the edited mask).
        CameraMode cameraMode;     // Free, Game or a preset
        std::uint32_t lookAtTarget; // packed replay id, 0 = None
        std::uint32_t attachTarget;
        std::uint32_t focusTarget;
        Vec2 lookAtOffset;          // screen framing, -1..1
        float lookAtRoll;           // degrees; also the preset roll
        float presetDistanceScale;  // 0..1 maps to 0.5x..1.5x of the preset distance
        float dofIntensity;         // 0..100
        float focusDistance;        // metres
        MarkerType markerType;
        Blend blend;
        std::uint8_t ease;          // 0 or 1
        AttachType attachType;
        ShakeType shake;
        std::uint8_t shakeIntensity;  // 1..10
        std::uint8_t shakeSpeedIndex; // index in kMarkerSpeedPercent
        std::uint8_t speedIndex;      // index in kMarkerSpeedPercent
        DofMode dof;
        FocusMode focus;
        std::uint8_t legacyCurve;     // 1: converted old clip, the UI says "Curved (old clip)"
        std::uint8_t transition;      // 0 none, 1 fade (in on the first marker, out on the last)
        std::uint16_t transitionMs;   // the fade's length
        std::uint8_t filter;          // effects: index in kMarkerFilters (0 none)
        std::uint8_t filterIntensity; // 0..100
        std::int8_t saturation;       // -100..100
        std::int8_t contrast;
        std::int8_t vignette;
        std::int8_t brightness;
        std::uint8_t shakeMotion;     // ShakeMotion; always 0 now, the shake is in cameraKeyframeShake
        std::uint8_t markerOrigin;    // 0 placed in the editor, 1 added by the shake migration
        std::uint8_t reservedFx[6];
    };

    // Marker speed and shake speed presets (percent); index 4 is 100 %.
    constexpr std::uint32_t kMarkerSpeedPercent[] = {5, 20, 35, 50, 100, 125, 150, 175, 200};
    constexpr std::uint32_t kMarkerSpeedCount = sizeof(kMarkerSpeedPercent) / sizeof(kMarkerSpeedPercent[0]);
    // Marker filters: the game's own photo mode filters, played
    // as animated post effects (ANIMPOSTFX_PLAY + _ANIMPOSTFX_SET_STRENGTH;
    // names from femga/rdr3_discoveries graphics/animpostfx). The index is
    // what the clip stores; 0 is none.
    constexpr const char* kMarkerFilters[] = {
        "None", "Game 01", "Game 02", "Game 03", "Game 04", "Game 05", "Game 06", "Game 07", "Game 08", "Game 09",
        "Game 10", "Game 11", "Game 12", "Game 13", "Game 14", "Game 15", "Game 16", "Game 17", "Game 18",
        "Modern 01", "Modern 02", "Modern 03", "Modern 04", "Modern 05", "Modern 06", "Modern 07", "Modern 08",
        "Modern 09", "Modern 10", "Vintage 01", "Vintage 02", "Vintage 03", "Vintage 04", "Vintage 05", "Vintage 06",
        "Vintage 07", "Vintage 08", "Vintage 09", "Vintage 10"};
    constexpr const char* kMarkerFilterPostFx[] = {
        "", "PhotoMode_FilterGame01", "PhotoMode_FilterGame02", "PhotoMode_FilterGame03", "PhotoMode_FilterGame04",
        "PhotoMode_FilterGame05", "PhotoMode_FilterGame06", "PhotoMode_FilterGame07", "PhotoMode_FilterGame08",
        "PhotoMode_FilterGame09", "PhotoMode_FilterGame10", "PhotoMode_FilterGame11", "PhotoMode_FilterGame12",
        "PhotoMode_FilterGame13", "PhotoMode_FilterGame14", "PhotoMode_FilterGame15", "PhotoMode_FilterGame16",
        "PhotoMode_FilterGame17", "PhotoMode_FilterGame18", "PhotoMode_FilterModern01", "PhotoMode_FilterModern02",
        "PhotoMode_FilterModern03", "PhotoMode_FilterModern04", "PhotoMode_FilterModern05",
        "PhotoMode_FilterModern06", "PhotoMode_FilterModern07", "PhotoMode_FilterModern08",
        "PhotoMode_FilterModern09", "PhotoMode_FilterModern10", "PhotoMode_FilterVintage01",
        "PhotoMode_FilterVintage02", "PhotoMode_FilterVintage03", "PhotoMode_FilterVintage04",
        "PhotoMode_FilterVintage05", "PhotoMode_FilterVintage06", "PhotoMode_FilterVintage07",
        "PhotoMode_FilterVintage08", "PhotoMode_FilterVintage09", "PhotoMode_FilterVintage10"};
    constexpr std::uint32_t kMarkerFilterCount = sizeof(kMarkerFilters) / sizeof(kMarkerFilters[0]);
    constexpr std::uint32_t kMarkerSpeedDefaultIndex = 4;
    constexpr float kMarkerFovMin = 10.0f;
    constexpr float kMarkerFovMax = 100.0f;
    constexpr float kMarkerFovDefault = 45.0f;
    constexpr float kMarkerFovStep = 1.0f;

    // Which fields a SetCameraKeyframeOptions command carries.
    enum CameraOptionField : std::uint32_t
    {
        CameraOptionMode = 1u << 0,
        CameraOptionLookAtTarget = 1u << 1,
        CameraOptionLookAtOffset = 1u << 2,
        CameraOptionLookAtRoll = 1u << 3,
        CameraOptionAttachTarget = 1u << 4,
        CameraOptionAttachType = 1u << 5,
        CameraOptionBlend = 1u << 6,
        CameraOptionEase = 1u << 7,
        CameraOptionShake = 1u << 8,           // Command::shake, the whole MarkerShake
        CameraOptionShakeIntensity = 1u << 9,  // retired (the bit stays reserved)
        CameraOptionShakeSpeed = 1u << 10,     // retired
        CameraOptionFov = 1u << 11,
        CameraOptionPresetDistance = 1u << 12,
        CameraOptionDof = 1u << 13,
        CameraOptionDofIntensity = 1u << 14,
        CameraOptionFocus = 1u << 15,
        CameraOptionFocusDistance = 1u << 16,
        CameraOptionFocusTarget = 1u << 17,
        CameraOptionMarkerType = 1u << 18,
        CameraOptionSpeed = 1u << 19,
        CameraOptionTransition = 1u << 20,
        CameraOptionTransitionMs = 1u << 21,
        CameraOptionFilter = 1u << 22,
        CameraOptionFilterIntensity = 1u << 23,
        CameraOptionSaturation = 1u << 24,
        CameraOptionContrast = 1u << 25,
        CameraOptionVignette = 1u << 26,
        CameraOptionBrightness = 1u << 27,
        CameraOptionShakeMotion = 1u << 28, // retired
        CameraOptionShakeCustom = 1u << 29, // retired
        CameraOptionSpline = 1u << 30,      // Command::spline
        CameraOptionExtra = 1u << 31        // Command::extra (CameraExtraField bits)
    };

    // The marker fields that came after the 72 byte options: the bone of the
    // attach and Look At targets (camera_bones.h reference: 0 root, named,
    // or a skeleton index), the attach pose as numbers (metres and degrees
    // in the attach frame, the same values Edit Camera stores), the near
    // clip plane and the DOF's own numbers. `fields` says which ones a
    // command carries (CameraExtraField); the snapshot fills all.
    enum CameraExtraField : std::uint32_t
    {
        CameraExtraAttachBone = 1u << 0,
        CameraExtraLookAtBone = 1u << 1,
        CameraExtraRelPosition = 1u << 2,
        CameraExtraRelRotation = 1u << 3,
        CameraExtraNearClip = 1u << 4,
        CameraExtraDofFStop = 1u << 5,
        CameraExtraDofLens = 1u << 6,
    };
    // Near clip (metres). 0 in a marker is the default, 0.1 m (a free
    // camera's usual value). The engine holds SET_CAM_NEAR_CLIP at 0.01 or
    // more (game build 1491.50: handler 0xF64210 > 0xF753A8, maxss with 0.01, stored
    // at [cam+0x84]).
    constexpr float kNearClipDefault = 0.1f;
    constexpr float kNearClipMin = 0.01f;
    constexpr float kNearClipMax = 1.0f;
    // DOF numbers (0 in a marker: from the intensity, a fixed mapping).
    constexpr float kDofFStopMin = 0.5f;
    constexpr float kDofFStopMax = 32.0f;
    constexpr float kDofLensMin = 10.0f;  // mm (35 mm film, 24 mm frame height)
    constexpr float kDofLensMax = 600.0f;
    struct CameraMarkerExtra
    {
        std::uint32_t fields;      // CameraExtraField bits (commands)
        std::uint32_t attachBone;  // camera_bones.h reference, 0 the root
        std::uint32_t lookAtBone;
        float nearClip;            // metres, 0 default
        float dofFStop;            // 0: from the intensity
        float dofLensMm;           // 0: from the FOV and the intensity
        Vec3 relPosition;          // attach frame, metres
        Vec3 relRotation;          // attach frame, degrees (x pitch, y roll, z heading)
    };

    // What the rendered camera got this tick, read back from the camera
    // object.
    enum CameraLiveFlags : std::uint32_t
    {
        CameraLiveNearRead = 1u << 0,     // nearRead is the camera's [cam+0x84]
        CameraLiveDofActive = 1u << 1,    // a DOF block was sent this tick
        CameraLiveDofRead = 1u << 2,      // the dof*Read values are the camera's block
        CameraLiveFocusOverwritten = 1u << 3, // the engine changed the focus distance between two ticks
        CameraLiveFocusPaused = 1u << 4,  // the engine's autofocus is paused for our distance
        CameraLiveEditCamera = 1u << 5,   // the free camera (Edit Camera) renders
        // The photo mode freeze held the camera system when the lens last
        // changed (the change shows once it thaws).
        CameraLiveFrozen = 1u << 6,
    };
    struct CameraLiveInfo
    {
        std::uint32_t flags;
        std::uint32_t focusSource;  // camera::FocusSource
        float nearSet;
        float nearRead;
        float dofFStopSet;
        float dofFStopRead;
        float dofLensSet;
        float dofLensRead;
        float focusSet;
        float focusRead;
        float inFocusNear;          // metres, thin lens with a 0.03 mm circle (information)
        float inFocusFar;           // metres, 0 when it reaches infinity
    };

    // Every editable option of a marker; the editor sends only what changed
    // (mask). The pose itself still goes through UpdateCameraKeyframe.
    struct CameraKeyframeOptions
    {
        std::uint32_t mask;         // CameraOptionField bits
        CameraMode cameraMode;
        std::uint32_t lookAtTarget;
        std::uint32_t attachTarget;
        std::uint32_t focusTarget;
        Vec2 lookAtOffset;
        float lookAtRoll;
        float presetDistanceScale;
        float fov;
        float dofIntensity;
        float focusDistance;
        MarkerType markerType;
        Blend blend;
        std::uint8_t ease;
        AttachType attachType;
        ShakeType shake;
        std::uint8_t shakeIntensity;
        std::uint8_t shakeSpeedIndex;
        std::uint8_t speedIndex;
        DofMode dof;
        FocusMode focus;
        std::uint8_t transition;
        std::uint8_t filter;
        std::uint16_t transitionMs;
        std::uint8_t filterIntensity;
        std::int8_t saturation;
        std::int8_t contrast;
        std::int8_t vignette;
        std::int8_t brightness;
        std::uint8_t shakeMotion; // ShakeMotion
        std::uint8_t reservedFx[2];
    };

    // An entity a keyframe can target: a puppet of the clip.
    struct TargetInfo
    {
        std::uint32_t id; // packed replay id
        std::uint32_t kind; // entity::EntityKind
        char label[kTargetLabelCapacity];
    };

    struct Snapshot
    {
        std::uint32_t structSize;
        std::uint32_t version;
        std::uint64_t sequence;
        std::uint32_t flags;
        std::uint32_t playbackFrameIndex;
        std::uint32_t playbackFrameCount;
        std::uint32_t cameraKeyframeCount;
        std::uint32_t cameraKeyframeTotal;
        std::uint64_t playheadMs;
        std::uint64_t durationMs;
        CameraState camera;
        CameraKeyframe cameraKeyframes[kMaxCameraKeyframesInSnapshot];
        char status[128];
        std::uint32_t speedIndex;
        std::uint32_t speedPercent;
        std::uint32_t clipCount;
        std::uint32_t clipTotal;
        std::uint64_t clipListSequence; // grows each time the list is rebuilt
        char currentClip[kClipNameCapacity];
        ClipInfo clips[kMaxClipsInSnapshot]; // newest first
        std::uint32_t targetCount;
        std::uint32_t bufferHeldMs; // what the replay buffer holds now (0 when off)
        TargetInfo targets[kMaxTargetsInSnapshot];
        // The last clip written to ReplayClips: the editor saves
        // its thumbnail when the sequence grows.
        std::uint64_t savedClipSequence;
        char savedClip[kClipNameCapacity];
        // The replay buffer's settings.
        std::uint32_t bufferEnabled;
        std::uint32_t bufferSeconds;
        std::uint32_t bufferMB;
        // The recording's elapsed time (manual, or since the F4
        // mark inside the buffer), and the length of the last saved clip.
        std::uint32_t recordingMs;
        std::uint32_t savedClipMs;
        std::uint32_t editingMarker; // the marker in Edit Camera, 0 = none
        // Clip compression (RedemptionEditor.ini CompressClips and
        // CompressAfterDays), the compressor's progress and the disk use of
        // all the clips of the folder (not only the ones in `clips`).
        std::uint32_t compressEnabled;
        std::uint32_t compressDays;
        std::uint32_t compressState; // clipcompress::worker::State: 0 off, 1 idle, 2 running, 3 held
        std::uint32_t compressDone;
        std::uint32_t compressTotal;
        std::uint32_t compressedClips;
        std::uint64_t clipsDiskBytes; // as on disk
        std::uint64_t clipsBytes;     // the same clips uncompressed
        // The recording gate. recordPauseReasons: gate::Reason
        // bits while a recording's clock is stopped (1 menu, 2 cutscene,
        // 4 fade, 8 loading), 0 otherwise. The spans and cuts of the clip
        // in memory, for the timeline.
        std::uint32_t recordPauseReasons;
        std::uint32_t spanCount;
        std::uint32_t spanTotal;
        std::uint32_t cutCount;
        ClipSpan spans[kMaxSpansInSnapshot];
        std::uint32_t cuts[kMaxCutsInSnapshot];
        // The world notice of the clip being played (world_notice.h
        // NoticeKind and NoticeText): shown when worldNoticeSequence grows.
        std::uint32_t worldNoticeSequence;
        std::uint32_t worldNoticeKind;
        std::uint32_t worldChapterRecorded;
        std::uint32_t worldChapterNow;
        std::uint32_t worldDifferences;
        std::uint32_t worldApplied;
        // Both sides must come from the same build: the structSize check
        // refuses a mixed pair. clipLoadSequence grows at
        // each clip load, buffer play and recording (another clip, not a
        // rename or a first save of the same one); compressingClip, the file
        // the compressor works on now (empty when none).
        std::uint32_t clipLoadSequence;
        // The game's language, GET_CURRENT_LANGUAGE + 1 (0 not read yet): the
        // editor's language when the ini's Language is auto (i18n.h).
        std::uint32_t gameLanguage;
        char compressingClip[kClipNameCapacity];
        // The seek settle (seek_settle.h). seekLoadState: SeekLoadState.
        // While it is SeekLoadFrozen the editor keeps showing the last picture
        // it took before the jump, with a spinner (a frozen render phase);
        // seekLoadSequence grows at each frozen load (a new picture is not
        // taken until it ends). seekLoadFarBits: settle::FarBits;
        // seekLoadElapsedMs: since the jump.
        std::uint32_t seekLoadState;
        std::uint32_t seekLoadSequence;
        std::uint32_t seekLoadFarBits;
        std::uint32_t seekLoadElapsedMs;
        std::uint64_t seekLoadTargetMs;
        // The open project. projectActiveClip: the clip in playback (~0u none);
        // projectPlayheadMs: the playhead on the project timeline;
        // projectSequence grows with every change of the project,
        // projectListSequence with every read of the folder, and
        // projectMessageSequence with every message (a refused edit, a
        // save).
        std::uint32_t projectFlags;
        std::uint32_t projectClipCount;
        std::uint32_t projectActiveClip;
        std::uint32_t projectListCount;
        std::uint64_t projectTotalMs;
        std::uint64_t projectPlayheadMs;
        std::uint64_t projectSequence;
        std::uint64_t projectListSequence;
        std::uint32_t projectMessageSequence;
        // Grows at each session reset after a script restart
        // (a save loaded); the add-on closes its screens when it grows.
        std::uint32_t sessionResetSequence;
        char projectName[kProjectNameCapacity];
        char projectFile[kClipNameCapacity];
        char projectMessage[128];
        ProjectClipInfo projectClips[kMaxProjectClipsInSnapshot];
        ProjectInfo projects[kMaxProjectsInSnapshot];
        // The environment settings of the clip in playback (env_override.h).
        // envClockMinutes -1 and envWeather 0 are Recorded; envWeather is the
        // weather type hash; envRecordedStartS the recorded clock at the
        // clip's start (what the first step out of Recorded starts from).
        // envClockMinutes -2 and envWeather 0xFFFFFFFF
        // are Live (EnvironmentOverride::kClockLive, kWeatherLive).
        std::uint32_t envFlags;
        std::int32_t envClockMinutes;
        std::uint32_t envWeather;
        std::uint32_t envBlendS;
        std::uint32_t envCloudsLive;
        std::uint32_t envRecordedStartS;
        // The free camera's options (FreeCamField) and the world overlay.
        std::uint32_t freeCamFlags;   // FreeCamFlags
        std::uint32_t freeCamMotion;  // 0 Classic, 1 Drone
        std::uint32_t freeCamFollow;  // freecam::Follow
        std::uint32_t freeCamTarget;  // packed replay id, 0 none
        float freeCamValues[static_cast<std::size_t>(FreeCamField::Count)];
        std::uint32_t overlayFlags;   // OverlayFlags
        std::uint32_t reservedCine;
        // Each marker's shake (same index as cameraKeyframes), and the shake in
        // force at the playhead (what a new marker starts from; on 0 when
        // none plays).
        MarkerShake cameraKeyframeShake[kMaxCameraKeyframesInSnapshot];
        MarkerShake shakeAtPlayhead;
        // The Spline defaults, whether the camera
        // weight applies now (playing or exporting), and each marker's own
        // tuning (same index as cameraKeyframes).
        SplineTuning splineDefaults;
        std::uint32_t splineWeightActive;
        SplineTuning cameraKeyframeSpline[kMaxCameraKeyframesInSnapshot];
        // Each marker's bone, attach pose,
        // near clip and DOF numbers (same index as cameraKeyframes); per
        // target (same index as targets) its camera_bones.h family, the
        // named bones it has (bit n: kNamed[n]) and its skeleton's bone
        // count (0: no skeleton known); what the camera got this tick.
        CameraMarkerExtra cameraKeyframeExtra[kMaxCameraKeyframesInSnapshot];
        std::uint32_t targetFamily[kMaxTargetsInSnapshot];
        std::uint32_t targetBoneMask[kMaxTargetsInSnapshot];
        std::uint32_t targetBoneCount[kMaxTargetsInSnapshot];
        CameraLiveInfo cameraLive;
        // Named bones 32..63 of each target (bit n: kNamed[32 + n]).
        std::uint32_t targetBoneMaskHigh[kMaxTargetsInSnapshot];
        // RedemptionEditor.ini RecordingIndicator (rec_indicator.h Mode).
        std::uint32_t recordingIndicator;
    };

    // Snapshot::seekLoadState.
    enum SeekLoadState : std::uint32_t
    {
        SeekLoadNone = 0,
        SeekLoadSettling = 1, // time held, the picture live (a near jump or a scrub release)
        SeekLoadFrozen = 2,   // a far jump: the last picture frozen with a spinner
    };

    // SeekAbsolute's `enabled` while the timeline bar is dragged:
    // the drag is a scrub (cheap, no load) until its end, which loads once
    // where the mouse was released. 0 is a plain seek (a click, a marker).
    constexpr std::uint32_t kSeekPlain = 0;
    constexpr std::uint32_t kSeekDragging = 1;
    constexpr std::uint32_t kSeekDragEnd = 2;

    struct Command
    {
        std::uint32_t structSize;
        std::uint32_t version;
        CommandType type;
        std::uint32_t keyframeId;
        std::uint64_t timeMs;
        CameraState camera;
        Easing easing;
        Path path;
        std::int32_t direction;
        std::uint32_t enabled;
        char name[kClipNameCapacity];
        CameraMode mode;
        std::uint32_t target;
        CameraKeyframeOptions options; // SetCameraKeyframeOptions (API 8)
        // A marker's whole shake, for
        // SetCameraKeyframeOptions with CameraOptionShake.
        MarkerShake shake;
        // A marker's Spline tuning
        // (SetCameraKeyframeOptions with CameraOptionSpline) or the defaults
        // (SetSplineDefaults).
        SplineTuning spline;
        // SetCameraKeyframeOptions with CameraOptionExtra.
        CameraMarkerExtra extra;
    };

    struct SharedCommandSlot
    {
        volatile LONG ready;
        std::uint32_t reserved;
        alignas(8) volatile LONG64 epoch;
        Command command;
    };

    // This mapping is owned independently by ReplayProbe and ReplayEditor.
    // Neither side stores or invokes a function pointer from the other module,
    // so an ASI hot-unload cannot leave ReShade executing unmapped core code.
    struct SharedTransport
    {
        std::uint32_t structSize;
        std::uint32_t magic;
        std::uint32_t transportVersion;
        std::uint32_t apiVersion;
        std::uint32_t processId;
        std::uint32_t reserved;
        alignas(8) volatile LONG64 epoch;
        volatile LONG online;
        volatile LONG snapshotSequence;
        volatile LONG commandRead;
        volatile LONG commandWrite;
        volatile LONG commandOverflowed;
        LONG reservedAtomic;
        Snapshot snapshot;
        SharedCommandSlot commands[kSharedCommandCapacity];
    };

    inline bool FormatSharedTransportName(
        wchar_t* destination,
        std::size_t capacity,
        std::uint32_t processId) noexcept
    {
        if (!destination || capacity == 0)
            return false;
        const int written = swprintf_s(
            destination,
            capacity,
            L"Local\\RedemptionEditor.EditorBridge.v%lu.%lu",
            static_cast<unsigned long>(kSharedTransportVersion),
            static_cast<unsigned long>(processId));
        return written > 0 && static_cast<std::size_t>(written) < capacity;
    }

    inline LONG AtomicRead(volatile LONG* value) noexcept
    {
        return InterlockedCompareExchange(value, 0, 0);
    }

    inline LONG64 AtomicRead64(volatile LONG64* value) noexcept
    {
        return InterlockedCompareExchange64(value, 0, 0);
    }

    inline bool SharedTransportHeaderValid(
        const SharedTransport& transport,
        std::uint32_t processId) noexcept
    {
        return transport.structSize == sizeof(SharedTransport) &&
            transport.magic == kSharedTransportMagic &&
            transport.transportVersion == kSharedTransportVersion &&
            transport.apiVersion == kApiVersion &&
            transport.processId == processId;
    }

    inline bool SharedTransportOnline(
        SharedTransport& transport,
        std::uint32_t processId) noexcept
    {
        if (AtomicRead(&transport.online) == 0)
            return false;
        return SharedTransportHeaderValid(transport, processId) &&
            AtomicRead64(&transport.epoch) != 0 &&
            AtomicRead(&transport.online) != 0;
    }

    inline void InitializeSharedTransport(
        SharedTransport& transport,
        std::uint32_t processId,
        std::uint64_t epoch) noexcept
    {
        InterlockedExchange(&transport.online, 0);
        InterlockedExchange(&transport.snapshotSequence, 1);
        transport.structSize = sizeof(SharedTransport);
        transport.magic = kSharedTransportMagic;
        transport.transportVersion = kSharedTransportVersion;
        transport.apiVersion = kApiVersion;
        transport.processId = processId;
        transport.reserved = 0;
        InterlockedExchange64(
            &transport.epoch,
            static_cast<LONG64>(epoch ? epoch : 1));
        InterlockedExchange(&transport.commandRead, 0);
        InterlockedExchange(&transport.commandWrite, 0);
        InterlockedExchange(&transport.commandOverflowed, 0);
        transport.reservedAtomic = 0;
        std::memset(&transport.snapshot, 0, sizeof(transport.snapshot));
        for (SharedCommandSlot& slot : transport.commands)
        {
            InterlockedExchange(&slot.ready, 0);
            slot.reserved = 0;
            InterlockedExchange64(&slot.epoch, 0);
            std::memset(&slot.command, 0, sizeof(slot.command));
        }
        InterlockedExchange(&transport.snapshotSequence, 2);
        InterlockedExchange(&transport.online, 1);
    }

    inline void SetSharedTransportOffline(SharedTransport& transport) noexcept
    {
        InterlockedExchange(&transport.online, 0);
    }

    inline bool PublishSharedSnapshot(
        SharedTransport& transport,
        const Snapshot& source,
        std::uint32_t processId) noexcept
    {
        if (!SharedTransportOnline(transport, processId))
            return false;
        InterlockedIncrement(&transport.snapshotSequence);
        transport.snapshot = source;
        transport.snapshot.structSize = sizeof(Snapshot);
        transport.snapshot.version = kApiVersion;
        InterlockedIncrement(&transport.snapshotSequence);
        return AtomicRead(&transport.online) != 0;
    }

    inline bool ReadSharedSnapshot(
        SharedTransport& transport,
        Snapshot& destination,
        std::uint32_t processId) noexcept
    {
        if (!SharedTransportOnline(transport, processId))
            return false;

        for (unsigned int attempt = 0; attempt < 4; ++attempt)
        {
            const LONG sequenceBefore =
                AtomicRead(&transport.snapshotSequence);
            if ((sequenceBefore & 1) != 0)
                continue;
            const LONG64 epochBefore = AtomicRead64(&transport.epoch);
            // Read straight into `destination`: the snapshot is over 130 KB,
            // too much for a second copy on the reader's stack. A torn read
            // returns false and the caller keeps its last good snapshot
            // elsewhere.
            destination = transport.snapshot;
            const LONG sequenceAfter =
                AtomicRead(&transport.snapshotSequence);
            const LONG64 epochAfter = AtomicRead64(&transport.epoch);
            if (sequenceBefore == sequenceAfter &&
                (sequenceAfter & 1) == 0 &&
                epochBefore == epochAfter &&
                epochAfter != 0 &&
                AtomicRead(&transport.online) != 0 &&
                destination.structSize == sizeof(Snapshot) &&
                destination.version == kApiVersion)
            {
                return true;
            }
        }
        return false;
    }

    // A probe loading during a submit (hot reload) re-initializes the
    // mapping: epoch changes, indices and slots go back to zero. A submit
    // that passed its epoch check just before must not publish its old
    // `write + 1` into the new session, or the consumer waits forever on a
    // slot that is never ready. The index is published with a
    // compare-exchange on the value this submit read, and the consumer
    // skips a slot below commandWrite that is not ready (only such a stale
    // publish leaves one: the single producer makes a slot ready before it
    // moves the index).
    // `beforePublish` is the test seam between the checks and the commit.
    template <typename BeforePublish>
    inline bool SubmitSharedCommandWith(
        SharedTransport& transport,
        const Command& command,
        std::uint32_t processId,
        BeforePublish&& beforePublish) noexcept
    {
        if (!SharedTransportOnline(transport, processId) ||
            command.structSize != sizeof(Command) ||
            command.version != kApiVersion ||
            command.type == CommandType::None)
        {
            return false;
        }

        const LONG64 epoch = AtomicRead64(&transport.epoch);
        const std::uint32_t read = static_cast<std::uint32_t>(
            AtomicRead(&transport.commandRead));
        const std::uint32_t write = static_cast<std::uint32_t>(
            AtomicRead(&transport.commandWrite));
        if (write - read >= kSharedCommandCapacity)
        {
            InterlockedExchange(&transport.commandOverflowed, 1);
            return false;
        }

        SharedCommandSlot& slot =
            transport.commands[write % kSharedCommandCapacity];
        InterlockedExchange(&slot.ready, 0);
        InterlockedExchange64(&slot.epoch, epoch);
        slot.command = command;
        InterlockedExchange(&slot.ready, 1);
        if (AtomicRead64(&transport.epoch) != epoch ||
            AtomicRead(&transport.online) == 0)
        {
            InterlockedExchange(&slot.ready, 0);
            return false;
        }
        beforePublish();
        // A reset in between moved commandWrite: this command belonged to
        // the old session and is dropped (the reset already cleared its slot).
        return InterlockedCompareExchange(
                   &transport.commandWrite,
                   static_cast<LONG>(write + 1),
                   static_cast<LONG>(write)) == static_cast<LONG>(write);
    }

    inline bool SubmitSharedCommand(
        SharedTransport& transport,
        const Command& command,
        std::uint32_t processId) noexcept
    {
        return SubmitSharedCommandWith(transport, command, processId, []() noexcept {});
    }

    inline bool TryPopSharedCommand(
        SharedTransport& transport,
        Command& command,
        std::uint32_t processId) noexcept
    {
        if (!SharedTransportOnline(transport, processId))
            return false;

        for (std::size_t attempt = 0;
            attempt < kSharedCommandCapacity;
            ++attempt)
        {
            const std::uint32_t read = static_cast<std::uint32_t>(
                AtomicRead(&transport.commandRead));
            const std::uint32_t write = static_cast<std::uint32_t>(
                AtomicRead(&transport.commandWrite));
            const std::uint32_t distance = write - read;
            if (distance == 0)
                return false;
            if (distance > kSharedCommandCapacity)
            {
                InterlockedExchange(
                    &transport.commandRead,
                    static_cast<LONG>(write));
                InterlockedExchange(&transport.commandOverflowed, 1);
                return false;
            }

            SharedCommandSlot& slot =
                transport.commands[read % kSharedCommandCapacity];
            if (AtomicRead(&slot.ready) == 0)
            {
                // Below commandWrite, so a hole left by an old session's
                // publish after a reset (see
                // SubmitSharedCommandWith): skipped instead of blocking the
                // queue for good.
                InterlockedCompareExchange(
                    &transport.commandRead,
                    static_cast<LONG>(read + 1),
                    static_cast<LONG>(read));
                continue;
            }
            const LONG64 slotEpoch = AtomicRead64(&slot.epoch);
            const Command candidate = slot.command;
            InterlockedExchange(&slot.ready, 0);
            InterlockedExchange(
                &transport.commandRead,
                static_cast<LONG>(read + 1));

            if (slotEpoch != AtomicRead64(&transport.epoch) ||
                candidate.structSize != sizeof(Command) ||
                candidate.version != kApiVersion ||
                candidate.type == CommandType::None)
            {
                continue;
            }
            command = candidate;
            return true;
        }
        return false;
    }

    inline bool ConsumeSharedCommandOverflow(
        SharedTransport& transport) noexcept
    {
        return InterlockedExchange(&transport.commandOverflowed, 0) != 0;
    }

    static_assert(std::is_standard_layout_v<Vec3> && std::is_trivially_copyable_v<Vec3>);
    static_assert(std::is_standard_layout_v<CameraState> &&
        std::is_trivially_copyable_v<CameraState>);
    static_assert(std::is_standard_layout_v<Vec2> && std::is_trivially_copyable_v<Vec2>);
    static_assert(std::is_standard_layout_v<CameraKeyframe> &&
        std::is_trivially_copyable_v<CameraKeyframe>);
    static_assert(sizeof(CameraKeyframe) == 128 && alignof(CameraKeyframe) == 8,
        "snapshot keyframe layout (API 8)");
    static_assert(std::is_standard_layout_v<CameraKeyframeOptions> &&
        std::is_trivially_copyable_v<CameraKeyframeOptions>);
    static_assert(sizeof(CameraKeyframeOptions) == 72, "options layout (API 9)");
    static_assert(std::is_standard_layout_v<ProjectClipInfo> && sizeof(ProjectClipInfo) == 88, "project clip layout (API 13)");
    static_assert(std::is_standard_layout_v<ProjectInfo> && sizeof(ProjectInfo) == 152, "project info layout (API 13)");
    static_assert(std::is_standard_layout_v<MarkerShake> && std::is_trivially_copyable_v<MarkerShake> && sizeof(MarkerShake) == 64,
        "marker shake layout (API 24)");
    static_assert(std::is_standard_layout_v<ShakeCustom> && std::is_trivially_copyable_v<ShakeCustom> && sizeof(ShakeCustom) == 32,
        "custom shake layout (API 19)");
    static_assert(std::is_standard_layout_v<SplineTuning> && std::is_trivially_copyable_v<SplineTuning> && sizeof(SplineTuning) == 20,
        "spline tuning layout (API 21)");
    static_assert(std::is_standard_layout_v<CameraMarkerExtra> && std::is_trivially_copyable_v<CameraMarkerExtra> &&
        sizeof(CameraMarkerExtra) == 48, "marker extra layout (API 22)");
    static_assert(std::is_standard_layout_v<CameraLiveInfo> && sizeof(CameraLiveInfo) == 48, "camera live layout (API 22)");
    static_assert(std::is_standard_layout_v<Snapshot> &&
        std::is_trivially_copyable_v<Snapshot>);
    static_assert(std::is_standard_layout_v<Command> &&
        std::is_trivially_copyable_v<Command>);
    static_assert(std::is_standard_layout_v<SharedCommandSlot> &&
        std::is_trivially_copyable_v<SharedCommandSlot>);
    static_assert(std::is_standard_layout_v<SharedTransport> &&
        std::is_trivially_copyable_v<SharedTransport>);
}
