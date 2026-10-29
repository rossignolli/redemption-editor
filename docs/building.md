# Building from source

Everything builds with Visual Studio 2022 (toolset v143), configuration **Release**, platform **x64**.

## 1. What you need

- Visual Studio 2022 with the "Desktop development with C++" workload and a Windows 10 or 11 SDK.
- Git, and Python 3 for the checks in `tools/`.
- The ReShade sources, as a submodule:

  ```
  git clone --recursive <this repository>
  git submodule update --init external/reshade
  ```

  The submodule is pinned to ReShade 6.6.2 (add-on API 18). The add-on uses its headers and Dear ImGui; the mod
  compiles MinHook from it.
- The Script Hook RDR2 SDK (next section).
- FFmpeg, for the editor core only (section 3).

## 2. Script Hook RDR2 SDK

The SDK may not be redistributed (its readme: "THIS ARCHIVE REDISTRIBUTION IS NOT ALLOWED"), so it is not in this
repository. Download it from http://www.dev-c.com/rdr2/scripthookrdr2/ and extract it so that these files exist:

```
external\ScriptHookRDR2_SDK\inc\main.h
external\ScriptHookRDR2_SDK\inc\nativeCaller.h
external\ScriptHookRDR2_SDK\inc\enums.h
external\ScriptHookRDR2_SDK\inc\types.h
external\ScriptHookRDR2_SDK\inc\natives.h
external\ScriptHookRDR2_SDK\lib\ScriptHookRDR2.lib
```

The folder is ignored by git. To keep the SDK elsewhere, set the MSBuild property `ScriptHookRdr2SdkDir` to the
folder that holds `inc\` and `lib\` (with a trailing backslash). When neither is set, the build looks two folders up
from the repository (the layout of the SDK's own `samples\` folder).

**natives.h and types.h.** The code uses the native names of the community native database, not the SDK's original
`natives.h`:

1. Generate `natives.h` at https://alloc8or.re/rdr3/nativedb/ (the "Generate natives.h" option) and put it in place of
   the SDK's `inc\natives.h`. Its first lines read `// https://alloc8or.re/rdr3/nativedb/` and it declares the
   natives with `NATIVE_DECL`.
2. Make sure `inc\types.h` defines the handle types that file uses. The one the project is built with has:

   ```cpp
   using Any = uint64_t;
   using Void = uint64_t;
   using ScrHandle = int;
   using AnimScene = int;
   using Blip = int;
   using Cam = int;
   using Entity = ScrHandle;
   using FireId = int;
   using Hash = unsigned int;
   using Interior = int;
   using ItemSet = ScrHandle;
   using Object = ScrHandle;
   using Ped = ScrHandle;
   using PersChar = ScrHandle;
   using Pickup = int;
   using Player = unsigned int;
   using PopZone = int;
   using Prompt = int;
   using PropSet = int;
   using Vehicle = ScrHandle;
   using Volume = ScrHandle;
   ```

   plus `struct Vector3` with three `float` members each aligned to 8 bytes (`sizeof(Vector3) == 24`), as in the SDK.

`RedemptionEditor.props` stops the build with a clear error when `inc\main.h` or `inc\natives.h` is missing.

## 3. FFmpeg (editor core only)

The video export links the FFmpeg libraries (libavcodec, libavformat, libavfilter, libavutil, libswscale,
libswresample) with x264, x265, NVENC, AMF, Quick Sync, AV1 (aom), VP8/9, Opus, Vorbis, LAME and zlib. They are built
once with vcpkg from `external/ffmpeg/vcpkg.json` (pinned baseline) and the release-only triplet in
`external/ffmpeg/triplets`:

```
git clone https://github.com/microsoft/vcpkg C:\vcpkg
tools\ffmpeg\install_ffmpeg.bat C:\vcpkg
```

It takes about an hour and installs into `external\ffmpeg\installed\x64-windows-release\` (ignored by git). The
optional second argument is a work folder (default `%TEMP%\re-vcpkg`); `VCPKG_MAX_CONCURRENCY` limits the parallel
jobs (default 4). To use FFmpeg from another place, set `FfmpegRoot` to the folder with `include\`, `lib\` and `bin\`.

The FFmpeg DLLs are delay-loaded from `RedemptionEditor\dlls\` next to `RDR2.exe`; the build copies them to
`<OutDir>RedemptionEditor\dlls\`. `py tools\ffmpeg\check_imports.py bin\Release\RedemptionEditorCore.dll` checks
that every FFmpeg import is still delay-loaded.
Without FFmpeg only `ReplayEditor.vcxproj` fails; the other projects build.

## 4. Building

Open `RedemptionEditor.sln` and build **Release | x64**, or from a Developer Command Prompt:

```
msbuild RedemptionEditor.sln /p:Configuration=Release /p:Platform=x64 /m
```

or one project at a time:

```
msbuild ReplayProbe.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64
```

| Project | Output | What it is |
|---|---|---|
| `ReplayProbe.vcxproj` | `RedemptionEditor.asi` | the mod (Script Hook RDR2 script) |
| `ReplayEditorLoader.vcxproj` | `RedemptionEditor.addon64` | the ReShade add-on that loads the editor core |
| `ReplayEditor.vcxproj` | `RedemptionEditorCore.dll`, `RedemptionEditor\` | the editor core: interface and video export |
| `ReplayCoreTests.vcxproj` | `ReplayCoreTests.exe` | the unit tests |
| `ReplayClipTool.vcxproj` | `ReplayClipTool.exe` | a command line tool that reads `.rpclip` files |
| `ReplayEditorUiRuntime.vcxproj`, `ReplayEditorUiTests.vcxproj` | `ReplayEditorUiTests.exe` | the UI harness |

The default output folder is `bin\Release\`. Do not use `/t:Rebuild` with a custom `OutDir`: the clean step can
delete files of an earlier build there; use `/t:Build`.

### Properties

Set them on the command line (`/p:Name=value`) or once per machine in `RedemptionEditor.user.props` next to the
projects (copy `RedemptionEditor.user.props.example`; the file is ignored by git).

| Property | Default | Meaning |
|---|---|---|
| `ScriptHookRdr2SdkDir` | `external\ScriptHookRDR2_SDK\` when present | the Script Hook RDR2 SDK (section 2) |
| `FfmpegRoot` | `external\ffmpeg\installed\x64-windows-release\` | the FFmpeg build (section 3) |
| `RDR2GameDir` | (none) | the game folder; when set, every build copies its output there |
| `DeployAfterBuild` | `true` | `false` skips that copy even when `RDR2GameDir` is set |
| `ReplaySymbolsDir` | (none) | when set, each build of the mod and of the core also keeps its binary, PDB and map under `<dir>\<VERSION>\<name>-<time>\` |
| `RedemptionDev` | `false` | `true` builds the developer build (below) |

The version comes from the `VERSION` file: a pre-build step of `ReplayProbe.vcxproj` writes
`src\version.generated.h` from it (`tools\generate_version_header.ps1`).

### The developer build

`/p:RedemptionDev=true` defines `REDEMPTION_DEV` in the mod, the core, the loader and the tests: the research probes are
compiled in and every ini key is read, not only the public ones. Use a separate `OutDir` and `IntDir` for it. Details:
[release/ini-and-dev-build.md](release/ini-and-dev-build.md).

## 5. Tests and checks

Run these from the repository root.

- **Unit tests.** `bin\Release\ReplayCoreTests.exe`. It prints each group and ends with
  `All retail-only core tests passed.` (exit code 0). Build the developer flavour too and run it the same way.
- **UI harness.** Build `ReplayEditorUiRuntime.vcxproj` first, then `ReplayEditorUiTests.vcxproj`, into the same
  `OutDir`. `ReplayEditorUiTests.exe --out <folder>` draws every editor screen at ten resolutions (from 1024x768 to
  3840x2160) with the real ImGui on a WARP device, checks the text layout and writes the PNGs, `report.json`,
  `findings.txt` and a contact sheet `index.html`. Options: `--filter <text>` (fixtures whose name contains it),
  `--res 1920x1080,2560x1440`, `--frames N`, `--no-png`, `--dump`, `--flat` (without the painted art). Exit code 0:
  no error-level finding.
- **Public build check.** `py tools\check_public_build.py` checks that the public build reads only the keys of
  `src/ini_public.h` and that `dist/RedemptionEditor.ini` matches the shipped text (`--write-ini` rewrites it).
- **Architecture audit.** `py tools\audit_retail_only_architecture.py` fails when removed research modules or a
  manual actor path come back, and checks `VERSION` against its accepted list.
- **Configuration page.** `py tools\release\gen_configuration_doc.py --check` fails when
  [configuration.md](configuration.md) is stale; run it without `--check` after a change to `src/ini_public.h`.

## 6. Packaging

`py tools\release\package_release.py --bin <OutDir> [--ffmpeg-root <FfmpegRoot>] [--out <folder>]` makes `RedemptionEditor-<VERSION>.zip` from a
public build: `RedemptionEditor.asi`, `RedemptionEditor.addon64`, `RedemptionEditorCore.dll`, the
`RedemptionEditor\dlls`, `fonts` and `ui` folders, `RedemptionEditor.ini` (from `dist/`), `README.md`, `LICENSE`,
`THIRD_PARTY_NOTICES.md`, the licences of the FFmpeg build (`THIRD_PARTY_LICENSES\`) and `SHA256SUMS.txt`. It
refuses a developer build and an ASI whose version is not the one in `VERSION`.

## 7. Running your build

Copy the outputs into the game folder, or set `RDR2GameDir` and build. Script Hook RDR2 can reload `.asi` files while
the game runs (Ctrl+R, with an empty `ScriptHookRDR2.dev` file in the game folder). The add-on loads a copy of
`RedemptionEditorCore.dll`, so the core can be replaced while the game runs; `RedemptionEditor.addon64` itself is read
by ReShade only at game start.
