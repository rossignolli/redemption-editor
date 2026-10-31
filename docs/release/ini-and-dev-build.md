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
| ... engine safety switches (support switches) | 52 | not in the shipped ini, all on; `kSupportSwitches`, listed for support in [troubleshooting-keys.md](troubleshooting-keys.md) |
| Developer keys | 26 | table in section 6 |

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
  lambdas of `WriteLogHeader`). The editor side has no developer keys left (the ini keys
  review).
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
| `CategoryStateLog` | RedemptionEditor | Dev probe (pinned) | `0` | CATEGORY_CTL lines |
| `ClipSaveFailTest` | RedemptionEditor | Dev probe (pinned) | `0` | forces a clip save failure (test) |
| `Debug` | RedemptionEditor | Dev probe (pinned) | `0` | developer mode: text HUD, direct keys, full log |
| `GameListenerLog` | RedemptionEditor | Dev probe (pinned) | `0 (MixProbe)` | LISTENER lines (default follows MixProbe) |
| `HorseAudioEvidence` | RedemptionEditor | Dev probe (pinned) | `0` | horse audio evidence lines |
| `ObjectDrawnCheck` | RedemptionEditor | Dev probe (pinned) | `0` | drawn check of posed objects (read only) |
| `PoseFidelity` | RedemptionEditor | Dev probe (pinned) | `0` | pose fidelity judge |
| `ProfileSlowTickMs` | RedemptionEditor | Dev probe (pinned) | `50` | profiler slow tick threshold |
| `SigDumpCode` | RedemptionEditor | Dev probe (pinned) | `0` | writes the exe's code and data to %LOCALAPPDATA%\RedemptionEditor\code for sigtool (SIG_DUMP) |
| `SigForceScan` | RedemptionEditor | Dev probe (pinned) | `0` | searches every game signature even on the 1491.50 exe (SIG lines) |
| `SoundCensus` | RedemptionEditor | Dev probe (pinned) | `0` | sound_census.cpp also runs SoundCapture (on) |
| `SoundCheck` | RedemptionEditor | Dev probe (pinned) | `-1` | sound check report (Debug=1) |
| `SoundFxProbe` | RedemptionEditor | Dev probe (pinned) | `0` | soundfx_probe.cpp also holds the speech-only scope the generic sound uses |
| `SoundGenericMute` | RedemptionEditor | Dev probe (pinned) |  | generic sound mute window (measurement) |
| `SoundGenericTrace` | RedemptionEditor | Dev probe (pinned) |  | generic sound trace window |
| `ClipWorldWait` | RedemptionEditor | Dev | `0` |  |
| `ClipWorldWaitMs` | RedemptionEditor | Dev | `20000` |  |
| `MapObjectsWaitMs` | RedemptionEditor | Dev | `3000` |  |
| `SoundGenericEntityDrivenSkip` | RedemptionEditor | Dev | `kEntityDrivenSkipDefault` |  |
| `SoundGenericSkip` | RedemptionEditor | Dev | `release` |  |
| `SoundGenericSlots` | RedemptionEditor | Dev | `64` |  |
| `SoundGenericTake` | RedemptionEditor | Dev |  |  |
