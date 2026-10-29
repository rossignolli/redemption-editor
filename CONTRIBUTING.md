# Contributing

Thanks for helping. A good bug report with a report ZIP is worth as much as code, and a fixed line in a translation
is as welcome as a new detour. This page is the long version of the README's
[Contributing](README.md#contributing) section.

## Reporting a bug

Support happens on [GitHub Issues](https://github.com/rossignolli/redemption-editor/issues/new/choose) only, so
nothing gets lost. Every report starts in game:

1. While the replay plays, press **Shift+F2** at the moment something looks or sounds wrong (a mark). Type a few words
   and press Enter. Several marks are fine.
2. Press **Shift+F1**: the mod saves a report ZIP in `ReplayReports\` (the log, your ini, system info, your marks with
   their screenshots, the clips you marked, and the crash dumps of the last 7 days).
3. Open a **Bug report** or, after a crash, a **Crash report** (the editor tells you about a crash the next time the
   game starts). The forms ask for the mod version, the ReShade version, the renderer (Vulkan or DirectX 12), where you
   bought the game and the steps. Attach the ZIP; over 25 MB, share it from a cloud drive and paste the link.

[docs/reporting.md](docs/reporting.md) lists everything that goes in the ZIP. Keep `FullLog=1` (the default) when you
report: the short log hides most of what we need.

## Proposing a change

- **Small fixes** (a typo, a translation, an obvious bug with a clear cause): open a pull request directly.
- **Anything bigger** (a new feature, a new detour, a change to the clip format or the editor protocol): open an issue
  first and say what you want to do and which part of the engine it touches. It saves you from writing code that
  can't be merged.
- Work on a branch of your fork, one topic per pull request, with a title that says what changed for the player or the
  code (`Free camera: keep the roll after a cut`, not `fix stuff`).
- Clip format changes: give the section a new version next to its writer in `src/clip_file.cpp`, keep older clips
  opening (or say in the pull request why they can't), and add a round trip test in `tests/clip_file_tests.cpp`.
- Editor protocol changes (`src/editor_protocol.h`, `src/export_channel.h`): raise the version, since both sides
  refuse a layout they don't know.

## Build and test

[docs/building.md](docs/building.md) has the full setup (Visual Studio 2022, the ReShade submodule, the Script Hook
RDR2 SDK, FFmpeg through vcpkg). New logic goes in pure headers or files without natives where possible
(`*_core.h`, `*_rules.h`), with unit tests in `tests/` (add the file to `ReplayCoreTests.vcxproj` and its filters).
Keep native and engine calls in the files that already make them.

The tools you'll use most:

- `ReplayCoreTests.exe`: the unit tests, one group per area.
- `ReplayEditorUiTests.exe --out <folder>`: every editor screen at ten resolutions with the real ImGui on a WARP
  device. Options: `--lang <code>` and `--lang pseudo` (every text about 40% longer and accented), `--scale 80,100,125`,
  `--res 1920x1080,3840x2160`, `--filter <text>`, `--flat`, `--no-png`.
- `ReplayClipTool.exe`: reads clips and projects. `sections`, `export <clip> <out.json>`, `segments`, `events`,
  `objects`, `speech`, `sounds`, `frozen-poses`, `pack-verify` (packs and proves the round trip is byte exact),
  `trim`, `project-info`. Run it without arguments for the full list.
- The developer build (`/p:RedemptionDev=true`): the research probes, every ini key, and the in game bench
  (`*.scn` scenarios dropped in `ReplayBench\inbox\`, parsed by `src/bench_script.cpp`).
- In the log: `PUPPET_BLINK` and `PUPPET_BLINK_SUMMARY` (bodies that should be on screen and weren't drawn as
  recorded), the `*_SUMMARY` lines of each subsystem, and `CONFIG_BUILD` (which flavor runs).

## The rules

These are not style preferences. A pull request that breaks one can't be merged.

1. **No leaked or proprietary code or assets.** Nothing from leaked game source code, not even names or numbers in a
   comment, and no game files, dumps or extracted assets in the repository. Engine knowledge comes from your own
   reverse engineering of the retail game, from public research (native databases, rdr3_discoveries) or from
   measurements in game, and the code says what it relies on.
2. **Engine offsets need signature checks.** Use the lowest rung of the ladder that works
   ([docs/architecture.md](docs/architecture.md)): natives; guarded reads of proven layouts; guarded writes on objects
   the replay owns; signature checked calls to engine functions; signature checked detours that forward every call
   and change behaviour only for replay entities. Every rung above natives needs its offset or signature proven for
   build 1.0.1491.50, an SEH guard and a log line on its failure path, and a mismatch turns the feature off, never
   falls back to a guess. Never touch engine state from the render thread, and never call a native from a detour
   (copy into a ring buffer the script thread reads). Say in the pull request which rung the change uses and why the
   lower ones were not enough.
3. **Fixes don't get public ini keys.** A fix is on for everyone. Only a setting a player actually chooses goes in
   `src/ini_public.h` (`kShippedIni` and `kPublicReadKeys`), with its default equal to the code's. A new engine
   change that a player might need to turn off for support is a support switch (`kSupportSwitches`: read, on, never in
   the shipped ini). Everything else is read through `DevIniInt`/`DevIniString` (`src/mod_identity.h`) and stays a
   developer key. After a change run `py tools\check_public_build.py --write-ini` and
   `py tools\release\gen_configuration_doc.py`.
4. **No network code.** The mod never connects to anything. Nothing leaves the player's PC unless they send it.
5. **The public build stays clean.** Research probes and the bench compile only under `REDEMPTION_DEV`;
   `tools/check_public_build.py` enforces it.

## Code style

- C++17, the style of the file you edit, four spaces, CRLF line endings. Match the surrounding code rather than
  importing your own conventions.
- Everything in English: identifiers, strings, comments, log lines, commit messages.
- Log lines are `NAME,key=value,...`. Tools and tests parse them, so don't change the format of an existing line; add
  a key or a new line instead.
- Threads you create are named (`src/thread_names.h`) and are plain Win32 threads (`std::thread` pins the module and
  breaks the Ctrl+R reload).

### Comments

Keep a comment only when the code can't say it by itself:

- why a non obvious choice was made: an invariant, an ordering, a threading or lifetime rule, a unit, a magic number;
- engine facts the code relies on: what an address, offset, signature or layout field means, why a guard exists and
  what breaks without it;
- a header of one to four lines saying what a file is for.

Leave out dates, version numbers, history ("it used to", "was fixed"), names of people or branches, references to
documents outside this repository, restated code and commented out code.

## Texts and translations

Every text a player sees goes through `TR("key", "English")` or `TRF("key", "English {value}", {{"value", ...}})`
(`src/i18n.h`); logs, ini keys and key names stay as they are. Never write a movable key (F4, Shift+F1...) as a
literal: use its `{key_<name>}` placeholder, which the mod fills in with the player's binding.

- After adding or changing a text, run `py tools\i18n\lang_table.py --write` to update `editor/lang/en.lang`, the
  translators' template. The mod never reads `en.lang`; it is generated, so don't edit it by hand.
- A translation is `editor/lang/<code>.lang` (UTF-8, the game's language codes). `--missing <code>` lists the keys it
  lacks, `--check` validates every file (an unknown key, a lost `{value}`, a missing key or a dash fails it).
- Check the layout with the UI harness `--lang <code>` and `--lang pseudo` after any layout change; both must report
  no error.

The current translations were made with AI help. Native speakers: corrections are very welcome, even one line at a
time.

## Pull request checklist

From the repository root, before you open the pull request:

- [ ] Every project of `RedemptionEditor.sln` builds (Release, x64), and so does the developer build of the mod, the
      core, the loader and the tests (`/p:RedemptionDev=true`, its own `OutDir`).
- [ ] `ReplayCoreTests.exe` of both builds ends with `All retail-only core tests passed.`, with new tests for new logic.
- [ ] After an interface change: `ReplayEditorUiTests.exe --out <folder>` exits 0 with no new finding in
      `findings.txt`, and you looked at the contact sheet at 1080p, 1440p and 4K (and with `--lang pseudo`).
- [ ] `py tools\check_public_build.py`, `py tools\audit_retail_only_architecture.py` and
      `py tools\release\gen_configuration_doc.py --check` pass.
- [ ] After a text change: `py tools\i18n\lang_table.py --write` and `--check`.
- [ ] Engine access: the rung is named in the description, with the signature check and the failure log line.
- [ ] No new public ini key for a fix.
- [ ] Tested in game what the change touches (record, play, scrub, export if it applies), and the description says
      what you tested and what you couldn't.

## License

Redemption Editor is under the GNU General Public License, version 3 ([LICENSE](LICENSE)). By opening a pull request
you agree that your contribution is licensed under the same terms. Third-party code needs a GPL-3.0 compatible license
and an entry in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
