# The public ini and the developer build

Redemption Editor is built in two flavours from the same sources:

- **the public build** (the default, the one in a release): it reads only the keys a player may need, listed in
  `src/ini_public.h`;
- **the developer build** (`/p:RedemptionDev=true`, which defines `REDEMPTION_DEV`): it also compiles the research
  probes and reads every other key, for testing and for measuring the engine.

The log header says which one runs on the `CONFIG_BUILD` line: `CONFIG_BUILD,flavor=public|dev,report_include_clips=0|1`.
`ASI_IDENTITY` carries the version, the SHA-256 and the size of the ASI, not the flavor.

## 1. The keys

| Class | Count | Where |
|---|---|---|
| Keys the public build reads | 129 | `src/ini_public.h` `kPublicReadKeys` |
| ... player settings of `[RedemptionEditor]`, `[Export]` and `[Keys]` | 52 | active lines of the shipped ini |
| ... editor preferences | 26 | commented lines of the shipped ini; the editor writes them (`FreeCam*`, `Spline*`, `CameraOverlay*`) |
| ... engine safety switches (support switches) | 51 | not in the shipped ini, all on; `kSupportSwitches`, listed for support in [troubleshooting-keys.md](troubleshooting-keys.md) |
| Developer keys | 213 | table in section 6 |

The shipped ini is `dist/RedemptionEditor.ini`, written from `kShippedIni` by `py tools/check_public_build.py
--write-ini`. The mod writes the same text on its first run when the game folder has no `RedemptionEditor.ini` (and
no `ReplayProbe.ini` of an older build); the log says `CONFIG_INI_DEFAULT,written=1`. Every value in it is the code's
default, so a player who never opens it gets what a missing key gives. Every shipped key is described in
[../configuration.md](../configuration.md); that page only says the support switches exist. A support switch is read
when a player adds its line by hand, as support asks.

## 2. How the public build reads the ini

- `src/mod_identity.h` has `kDevBuild` (true when `REDEMPTION_DEV` is defined) and `DevIniInt`, `DevIniIntW` and
  `DevIniString`. In the public build they return the default, exactly as `GetPrivateProfileInt`/`String` return it
  for a missing key, without opening the file; in the developer build they are `IniInt`/`IniString`.
- Every read of a key that is not public goes through them (`script.cpp`, `src/replay_events.cpp`,
  `src/sound_generic.cpp`, `src/sound_census.cpp`, `src/soundfx_probe.cpp`, and the `devKey`
  lambdas of `WriteLogHeader`). The editor side keeps its developer keys under `#if defined(REDEMPTION_DEV)`
  (`editor/render_export/render_export.cpp`: `AssumeLatency`, `Slots`, `Ffmpeg`, `Backend`, `FfmpegDlls`;
  `src/export_probe.cpp`: `TimerLead`; `editor/editor_loader.cpp`: `EditorGpuEvents`).
- `SectionFor` (and `SectionForW`) look in `[RedemptionEditor]`, then the old `[ReplayPoC]`, then `[Troubleshooting]`:
  a switch added under `[Troubleshooting]` (the section older shipped inis had) is read, and one under
  `[RedemptionEditor]` wins.
- `CONFIG_INI` logs the `[RedemptionEditor]` section as read.

## 3. The developer build

- The MSBuild property `RedemptionDev=true` defines `REDEMPTION_DEV` in `ReplayProbe.vcxproj`, `ReplayEditor.vcxproj`,
  `ReplayEditorLoader.vcxproj` and `ReplayCoreTests.vcxproj`. Build it into its own `OutDir` and `IntDir`.
- `ReplayProbe.vcxproj` compiles `src/volume_probe.cpp`, `src/world_sound_probe.cpp`, `src/mix_probe.cpp`,
  `src/speech_level_probe.cpp`, `src/water_probe.cpp` and `src/capture_probe.cpp` only in the developer build. Their
  headers give the public build inline no-ops, which is what each probe does with its key at 0.
- The other probes share their file with code that runs by default, so they stay compiled and their keys are pinned
  off in the public build (class "Dev probe (pinned)").
- Diagnostics stay in both builds: the crash log and minidumps, the Shift+F1 report ZIP, Shift+F2 marks (`QaMarks`),
  `FullLog`, and the summary lines (`CURSOR_STATE`, `DRAWSTORE_SLOT`, `SHAKE_MIGRATION`, `CAMERA_SHAKE`, `UI_ART`, the
  `*_SUMMARY`/`*_TOTAL` lines), kept with the default `FullLog=0` (`src/log_policy.h` `EssentialLine`). The public
  build's `SHAKE_MIGRATION` has the migration's counts and `proof=skipped`: the frame by frame proof of a legacy shake
  migration runs in the developer build only. The evidence logs that are on by default (class "Dev log (pinned on)")
  keep writing in the public build; only their keys are developer keys.

## 4. The report flow

See [../reporting.md](../reporting.md) for the player's side.

- **Crash files.** The crash log `RedemptionEditor.crash.log` and the minidumps in
  `%LOCALAPPDATA%\RedemptionEditor\crashes` (the last 5).
- **The crash notice** (`src/crash_notice.h`). On the first frame the editor answers, it lists `crashes\*.dmp`. A dump
  whose name (`RDR2_<build>_<time>_pid<pid>_t<thread>.dmp`) names another process than this one, written after the
  last dump already told and at most 7 days ago, shows the notice "Redemption Editor crashed last time" for 45 s, top
  centre: when it was, the report key, the folder the ZIP goes to, and that clips go in only when marked. The newest
  dump's time is then written to `%LOCALAPPDATA%\RedemptionEditor\crash-notice.txt`, so each crash is told once. A dump
  of the running process (a fault the game survived) is never told.
- **Clips only on request** (`src/qa_report.h` `ReportClips`). The ZIP carries the clips the marks name and, only with
  `ReportIncludeClips=1`, the clip open in the editor. `report.txt` says `clips: marked only` or
  `clips: marked and open (ReportIncludeClips=1)`; `REPORT_SAVE,phase=start` logs `clips=<n>,clips_policy=...`.
- `.github/ISSUE_TEMPLATE/crash_report.yml` and `bug_report.yml` ask for the version, the ReShade version, the
  renderer, what happened, the steps and the ZIP.

## 5. Checks

- `tools/check_public_build.py` (standard library) lists every ini read in the sources the public build compiles (the
  unconditional `ClCompile` items of the three shipped projects and every header of `src/` and `editor/`, outside
  `#if defined(REDEMPTION_DEV)`). It fails on a key outside `kPublicReadKeys`, on a public key nobody reads, on a key
  neither in the shipped ini nor a support switch, on a support switch that is shipped, off, not read, a developer key
  (section 6) or missing from [troubleshooting-keys.md](troubleshooting-keys.md), on a shipped (or support switch)
  value that differs from a literal default at its read site, on a read whose key it cannot name, and when
  `dist/RedemptionEditor.ini` differs from `kShippedIni`.
- `tests/ini_public_tests.cpp` (core tests): every public read is in the shipped ini or a support switch (never both)
  and every shipped line is read; the support switches are on, read and out of the shipped ini; `FullLog` and
  `QaMarks` ship; no probe key ships; the shipped `[Keys]` load to the default table; pinned
  defaults; `DevIniInt`/`DevIniString` in both builds; `[Troubleshooting]` read and overridden by `[RedemptionEditor]`;
  `WriteDefaultIni`; `ReportClips`; the crash notice decision and texts.
- `tools/release/gen_configuration_doc.py --check`: [../configuration.md](../configuration.md) matches `kShippedIni`.
- The UI harness draws the crash notice (fixtures `notices/crash` and `notices/crash-busy`) at every resolution.

## 6. Developer keys

Read only by the developer build; the public build uses the default. Classes: "Dev" (tuning and alternatives),
"Dev log (pinned on)" (evidence logs that write by default), "Dev probe (pinned)" (a probe compiled in both builds,
off in the public one), "Dev probe (compiled out)" (a probe compiled only in the developer build).

| Key | Section | Class | Default | Note |
|---|---|---|---|---|
| `CaptureProbe` | RedemptionEditor | Dev probe (compiled out) | `0` |  |
| `MixProbe` | RedemptionEditor | Dev probe (compiled out) | `0` |  |
| `SpeechLevelProbe` | RedemptionEditor | Dev probe (compiled out) | `0` |  |
| `VolumeProbe` | RedemptionEditor | Dev probe (compiled out) | `0` |  |
| `VolumeProbeCategories` | RedemptionEditor | Dev probe (compiled out) | `6` |  |
| `VolumeProbeEnv` | RedemptionEditor | Dev probe (compiled out) | `1` |  |
| `VolumeProbeSampleMs` | RedemptionEditor | Dev probe (compiled out) | `100` |  |
| `VolumeProbeTop` | RedemptionEditor | Dev probe (compiled out) | `1` |  |
| `VolumeProbeWindowMs` | RedemptionEditor | Dev probe (compiled out) | `500` |  |
| `VolumeProbeWorld` | RedemptionEditor | Dev probe (compiled out) | `8` |  |
| `WaterGridProbe` | RedemptionEditor | Dev probe (compiled out) | `0` |  |
| `WaterProbeAt` | RedemptionEditor | Dev probe (compiled out) |  |  |
| `WorldSoundProbe` | RedemptionEditor | Dev probe (compiled out) | `0 (VolumeProbe)` |  |
| `WorldSoundProbeGroupMs` | RedemptionEditor | Dev probe (compiled out) | `1000` |  |
| `CategoryStateLog` | RedemptionEditor | Dev probe (pinned) | `0` | CATEGORY_CTL lines |
| `CategoryStateWatch` | RedemptionEditor | Dev probe (pinned) | `kDefaultWatchText` | categories for CATEGORY_CTL |
| `ClipSaveFailTest` | RedemptionEditor | Dev probe (pinned) | `0` | forces a clip save failure (test) |
| `Debug` | RedemptionEditor | Dev probe (pinned) | `0` | developer mode: text HUD, direct keys, full log |
| `EditorGpuEvents` | RedemptionEditor | Dev probe (pinned) | `0` | GPU event markers (editor loader) |
| `FxMotionProbe` | RedemptionEditor | Dev probe (pinned) | `1` | FX_RELOCATE_PROBE / FX_REG_RECREATE evidence (on, capped) |
| `GameListenerLog` | RedemptionEditor | Dev probe (pinned) | `0 (MixProbe)` | LISTENER lines (default follows MixProbe) |
| `HorseAudioEvidence` | RedemptionEditor | Dev probe (pinned) | `0` | horse audio evidence lines |
| `ObjectDrawnCheck` | RedemptionEditor | Dev probe (pinned) | `0` | drawn check of posed objects (read only) |
| `ObjectLookEvidence` | RedemptionEditor | Dev probe (pinned) | `0` | object look evidence path |
| `PedVariablesReadProbe` | RedemptionEditor | Dev probe (pinned) | `0` | sound variable lookup probe |
| `PoseFidelity` | RedemptionEditor | Dev probe (pinned) | `0` | pose fidelity judge |
| `PoseFidelityAsync` | RedemptionEditor | Dev probe (pinned) | `1` | pose fidelity judge thread |
| `PoseFidelityHz` | RedemptionEditor | Dev probe (pinned) | `10` | pose fidelity judge rate |
| `ProfileSlowTickMs` | RedemptionEditor | Dev probe (pinned) | `50` | profiler slow tick threshold |
| `SoundCensus` | RedemptionEditor | Dev probe (pinned) | `0` | sound_census.cpp also runs SoundCapture (on) |
| `SoundCheck` | RedemptionEditor | Dev probe (pinned) | `-1` | sound check report (Debug=1) |
| `SoundCheckNotice` | RedemptionEditor | Dev probe (pinned) | `3` | sound check notice threshold |
| `SoundClock` | RedemptionEditor | Dev probe (pinned) | `1` | sound clock lines (on) |
| `SoundFxProbe` | RedemptionEditor | Dev probe (pinned) | `0` | soundfx_probe.cpp also holds the speech-only scope the generic sound uses |
| `SoundGenericMute` | RedemptionEditor | Dev probe (pinned) |  | generic sound mute window (measurement) |
| `SoundGenericTrace` | RedemptionEditor | Dev probe (pinned) |  | generic sound trace window |
| `WaterShapeLog` | RedemptionEditor | Dev probe (pinned) | `0` | water shape lines (F8) |
| `ClipWorldLog` | RedemptionEditor | Dev log (pinned on) | `1` |  |
| `EntityQueryLog` | RedemptionEditor | Dev log (pinned on) | `1` |  |
| `FootEventLog` | RedemptionEditor | Dev log (pinned on) | `1` |  |
| `MixScenePoolLog` | RedemptionEditor | Dev log (pinned on) | `1` |  |
| `MotionStateLog` | RedemptionEditor | Dev log (pinned on) | `1` |  |
| `PedAudioStateLog` | RedemptionEditor | Dev log (pinned on) | `1` |  |
| `PedSpeedReplayLog` | RedemptionEditor | Dev log (pinned on) | `1` |  |
| `SpeechDuckLog` | RedemptionEditor | Dev log (pinned on) | `1` |  |
| `TheatreLog` | RedemptionEditor | Dev log (pinned on) | `1` |  |
| `WindReplayLog` | RedemptionEditor | Dev log (pinned on) | `1` |  |
| `AssumeLatency` | Export | Dev | `-1` |  |
| `Backend` | Export | Dev | `libav` |  |
| `Ffmpeg` | Export | Dev |  |  |
| `FfmpegDlls` | Export | Dev |  |  |
| `Slots` | Export | Dev | `6` |  |
| `TimerLead` | Export | Dev | `1` |  |
| `AnimalBankHold` | RedemptionEditor | Dev | `1` |  |
| `AnimalBankLeadMs` | RedemptionEditor | Dev | `4000` |  |
| `AnimalVocalNative` | RedemptionEditor | Dev | `1` |  |
| `AnimalVocalRetry` | RedemptionEditor | Dev | `1` |  |
| `CategoryStateFields` | RedemptionEditor | Dev | `volume` |  |
| `CategoryStateKeepLive` | RedemptionEditor | Dev | `kDefaultKeepLiveText` |  |
| `CategoryStateRecord` | RedemptionEditor | Dev | `1` |  |
| `ClipStartBatch` | RedemptionEditor | Dev | `2` |  |
| `ClipStartSettleMs` | RedemptionEditor | Dev | `500` |  |
| `ClipStartStagger` | RedemptionEditor | Dev | `0` |  |
| `ClipStartStreamingMax` | RedemptionEditor | Dev | `8` |  |
| `ClipWorldWait` | RedemptionEditor | Dev | `0` |  |
| `ClipWorldWaitMs` | RedemptionEditor | Dev | `20000` |  |
| `DoorKeepOpenRatio` | RedemptionEditor | Dev | `1` |  |
| `DoorRebindMs` | RedemptionEditor | Dev | `250` |  |
| `EngineFeetInReplay` | RedemptionEditor | Dev | `0` |  |
| `EntityPoolReader` | RedemptionEditor | Dev | `1` |  |
| `FireAdoptLive` | RedemptionEditor | Dev | `0` |  |
| `FireUntrackedInitial` | RedemptionEditor | Dev | `0` |  |
| `FootBaseVolumeReplay` | RedemptionEditor | Dev | `1` |  |
| `FootEventRecord` | RedemptionEditor | Dev | `1` |  |
| `FootMaterialReplay` | RedemptionEditor | Dev | `1` |  |
| `FootMixReplay` | RedemptionEditor | Dev | `1` |  |
| `FootMovementRecord` | RedemptionEditor | Dev | `1` |  |
| `FxApplyRecordedFlags` | RedemptionEditor | Dev | `1` |  |
| `FxCheckMotion` | RedemptionEditor | Dev | `1` |  |
| `FxOwnerListImmediate` | RedemptionEditor | Dev | `1` |  |
| `FxOwnerRule` | RedemptionEditor | Dev | `1` |  |
| `FxRecordSettings` | RedemptionEditor | Dev | `1` |  |
| `FxRegisteredCallGaps` | RedemptionEditor | Dev | `1` |  |
| `FxRegisteredOwnerReady` | RedemptionEditor | Dev | `1` |  |
| `FxRegisteredParentOnce` | RedemptionEditor | Dev | `1` |  |
| `FxRegisteredRecreateBackoffMs` | RedemptionEditor | Dev | `250` |  |
| `FxRegisteredSplit` | RedemptionEditor | Dev | `1` |  |
| `FxWorldOwnerRule` | RedemptionEditor | Dev | `1` |  |
| `FxWorldSiteRule` | RedemptionEditor | Dev | `1` |  |
| `HatFix` | RedemptionEditor | Dev | `1` |  |
| `HatLook` | RedemptionEditor | Dev | `1` |  |
| `HideNeverAnimated` | RedemptionEditor | Dev | `1` |  |
| `HumanOwnVoiceFallback` | RedemptionEditor | Dev | `1` |  |
| `JitterFix` | RedemptionEditor | Dev | `1` |  |
| `ListenerCamMatchM` | RedemptionEditor | Dev | `1.0` |  |
| `ListenerMicFallbackHash` | RedemptionEditor | Dev | `0` |  |
| `ListenerRecord` | RedemptionEditor | Dev | `1` |  |
| `MapObjectsWaitMs` | RedemptionEditor | Dev | `3000` |  |
| `MetapedApply` | RedemptionEditor | Dev | `1` |  |
| `MetapedRecord` | RedemptionEditor | Dev | `1` |  |
| `MixSceneCodeSkip` | RedemptionEditor | Dev |  |  |
| `MixSceneEngineOpenFromPool` | RedemptionEditor | Dev | `1` |  |
| `MixSceneOwnPlayerState` | RedemptionEditor | Dev | `1` |  |
| `MixSceneRecord` | RedemptionEditor | Dev | `1` |  |
| `MixSceneRecordDirect` | RedemptionEditor | Dev | `1` |  |
| `MixSceneReplay` | RedemptionEditor | Dev | `0` |  |
| `MixSceneReplayCode` | RedemptionEditor | Dev | `0` |  |
| `MixSceneReplayConversation` | RedemptionEditor | Dev | `1` |  |
| `MixSceneReplayScript` | RedemptionEditor | Dev | `1` |  |
| `MixSceneVars` | RedemptionEditor | Dev | `1` |  |
| `MoonRecord` | RedemptionEditor | Dev | `1` |  |
| `MountPreRender` | RedemptionEditor | Dev | `0` |  |
| `MovementVolumeReplay` | RedemptionEditor | Dev | `1` |  |
| `MusicStop` | RedemptionEditor | Dev | `1` |  |
| `NoRecordAltAppearance` | RedemptionEditor | Dev | `0` |  |
| `NoRecordMapNow` | RedemptionEditor | Dev | `1` |  |
| `ObjectPosePlaces` | RedemptionEditor | Dev | `4` |  |
| `PedAudioKeepRecordedLength` | RedemptionEditor | Dev | `1` |  |
| `PedAudioLoopFollowsPuppet` | RedemptionEditor | Dev | `1` |  |
| `PedCapacity` | RedemptionEditor | Dev | `148 (WorldModel=gta), 56` |  |
| `PedSpeedRecord` | RedemptionEditor | Dev | `1` |  |
| `PlayerWeaponBank` | RedemptionEditor | Dev | `1` |  |
| `PoseLateBackoff` | RedemptionEditor | Dev | `0` |  |
| `PoseStallPollMs` | RedemptionEditor | Dev | `250` |  |
| `PoseWorkerPollMs` | RedemptionEditor | Dev | `5` |  |
| `PuppetAlwaysPrerender` | RedemptionEditor | Dev | `1` |  |
| `PuppetFoleyVariants` | RedemptionEditor | Dev | `0` |  |
| `PuppetFootLod` | RedemptionEditor | Dev | `1` |  |
| `PuppetForceAnimUpdate` | RedemptionEditor | Dev | `1` |  |
| `PuppetIncludeKeep` | RedemptionEditor | Dev |  |  |
| `PuppetLodMultiplier` | RedemptionEditor | Dev | `10` |  |
| `PuppetPersistentByEntry` | RedemptionEditor | Dev | `1` |  |
| `PuppetUnrecordedPersistent` | RedemptionEditor | Dev | `1` |  |
| `RecordPedAnimRadiusM` | RedemptionEditor | Dev | `40` |  |
| `RecordPedForceAnim` | RedemptionEditor | Dev | `0` |  |
| `RecordPedLodMultiplier` | RedemptionEditor | Dev | `0` |  |
| `RecordPedPrerender` | RedemptionEditor | Dev | `0` |  |
| `RegisteredFxOwnerCheck` | RedemptionEditor | Dev | `1` |  |
| `RenderTargetUiAppBlockInput` | RedemptionEditor | Dev | `1` |  |
| `RenderTargetUiApps` | RedemptionEditor | Dev | `SHOP_BROWSING` |  |
| `ReplayBufferAdaptive` | RedemptionEditor | Dev | `0` |  |
| `ReplayBufferMaxMB` | RedemptionEditor | Dev | `512` |  |
| `ReplayBufferReserveMB` | RedemptionEditor | Dev | `2048` |  |
| `ReplayCamDofBlend` | RedemptionEditor | Dev | `1` |  |
| `ReplayCamDofRecreate` | RedemptionEditor | Dev | `0` |  |
| `ReplayCameraCutMode` | RedemptionEditor | Dev | `bits` |  |
| `ReplayCameraRelease` | RedemptionEditor | Dev | `paused` |  |
| `ReplayCamFocusHysteresis` | RedemptionEditor | Dev | `1` |  |
| `ReplayCamFocusSmooth` | RedemptionEditor | Dev | `1` |  |
| `ReplayCamPrefetch` | RedemptionEditor | Dev | `1` |  |
| `ReplayCamPrefetchLeadMs` | RedemptionEditor | Dev | `1500` |  |
| `ReplayCamPrefetchMeters` | RedemptionEditor | Dev | `30` |  |
| `ReplayCamSmoothReframe` | RedemptionEditor | Dev | `1` |  |
| `ReplayCamTargetRecorded` | RedemptionEditor | Dev | `1` |  |
| `ReplayGameListener` | RedemptionEditor | Dev | `0` |  |
| `SceneryHandoffTicks` | RedemptionEditor | Dev | `1` |  |
| `SegmentSwitchInTick` | RedemptionEditor | Dev | `1` |  |
| `ShotAudioDelay` | RedemptionEditor | Dev | `1` |  |
| `ShotFxLegacyDedupe` | RedemptionEditor | Dev | `1` |  |
| `SoundCapture` | RedemptionEditor | Dev | `1` |  |
| `SoundGenericCoreBByRef` | RedemptionEditor | Dev | `1` |  |
| `SoundGenericDirectParams` | RedemptionEditor | Dev | `0` |  |
| `SoundGenericEntityDrivenSkip` | RedemptionEditor | Dev | `kEntityDrivenSkipDefault` |  |
| `SoundGenericEntityMode` | RedemptionEditor | Dev | `c` |  |
| `SoundGenericEnvGroup` | RedemptionEditor | Dev | `1` |  |
| `SoundGenericExport` | RedemptionEditor | Dev | `0` |  |
| `SoundGenericHoldMs` | RedemptionEditor | Dev | `150` |  |
| `SoundGenericKeepLoops` | RedemptionEditor | Dev | `1` |  |
| `SoundGenericMaxMBPerMin` | RedemptionEditor | Dev | `8` |  |
| `SoundGenericOriginSkip` | RedemptionEditor | Dev | `1` |  |
| `SoundGenericPlay` | RedemptionEditor | Dev | `1` |  |
| `SoundGenericPlayerRoute` | RedemptionEditor | Dev | `0` |  |
| `SoundGenericPreloadDelayMs` | RedemptionEditor | Dev | `100` |  |
| `SoundGenericPreloadMs` | RedemptionEditor | Dev | `500` |  |
| `SoundGenericSkip` | RedemptionEditor | Dev | `release` |  |
| `SoundGenericSlots` | RedemptionEditor | Dev | `64` |  |
| `SoundGenericTake` | RedemptionEditor | Dev |  |  |
| `SoundGenericUpdateHz` | RedemptionEditor | Dev | `20` |  |
| `SoundGenericVehicleBlock` | RedemptionEditor | Dev | `1` |  |
| `SoundGenericWeaponOffShot` | RedemptionEditor | Dev | `1` |  |
| `SoundGenericWorldCap` | RedemptionEditor | Dev | `1` |  |
| `SoundGenericWorldCapCount` | RedemptionEditor | Dev | `6` |  |
| `SoundQueueEnvGroup` | RedemptionEditor | Dev | `1` |  |
| `SoundStepSetGate` | RedemptionEditor | Dev | `1` |  |
| `SpawnedObjects` | RedemptionEditor | Dev | `1` |  |
| `SpawnedStaticScenery` | RedemptionEditor | Dev | `1` |  |
| `SpeechAudibleM` | RedemptionEditor | Dev | `60` |  |
| `SpeechDuckDerivedFlags` | RedemptionEditor | Dev | `kFlagEde` |  |
| `SpeechLineType` | RedemptionEditor | Dev | `1` |  |
| `SpeechPlayerOnPlayer` | RedemptionEditor | Dev | `1` |  |
| `SpeechPlayerScriptNear` | RedemptionEditor | Dev | `0` |  |
| `SpeechQueueAnyContext` | RedemptionEditor | Dev | `1` |  |
| `SpeechResumeWaitOld` | RedemptionEditor | Dev | `1` |  |
| `SpeechScriptPath` | RedemptionEditor | Dev | `0` |  |
| `StillPoseEngine` | RedemptionEditor | Dev | `1` |  |
| `TheatrePauseScriptHook` | RedemptionEditor | Dev | `0` |  |
| `TheatreReloadDelayMs` | RedemptionEditor | Dev | `0` |  |
| `TimecycleRecord` | RedemptionEditor | Dev | `1` |  |
| `TimecycleReplay` | RedemptionEditor | Dev | `1` |  |
| `TPoseOnScreenOnly` | RedemptionEditor | Dev | `1` |  |
| `TPoseSettleMs` | RedemptionEditor | Dev | `1500` |  |
| `TPoseWatch` | RedemptionEditor | Dev | `1` |  |
| `TrailPlayerFollow` | RedemptionEditor | Dev | `1` |  |
| `TrainPuppet` | RedemptionEditor | Dev | `0` |  |
| `TrainWorldClear` | RedemptionEditor | Dev | `0` |  |
| `WeaponFxEvents` | RedemptionEditor | Dev | `1` |  |
| `WorldClearFlags` | RedemptionEditor | Dev |  |  |
| `WorldFocus` | RedemptionEditor | Dev | `1` |  |
| `WorldModelStep` | RedemptionEditor | Dev | `3` |  |
| `WorldReloadPath` | RedemptionEditor | Dev | `slot` |  |
| `WorldScriptsCleanup` | RedemptionEditor | Dev | `1` |  |
