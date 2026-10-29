# Architecture

## Three binaries

| Binary | Project | Runs in | Job |
|---|---|---|---|
| `RedemptionEditor.asi` | `ReplayProbe.vcxproj` (`main.cpp`, `script.cpp`, `src/`) | Script Hook RDR2's script thread | recording, playback, the free camera, the world takeover, the clip files |
| `RedemptionEditor.addon64` | `ReplayEditorLoader.vcxproj` (`editor/editor_loader.cpp`) | ReShade | loads a copy of the core and forwards ReShade's events to it; swaps the copy between two frames when the core file changes |
| `RedemptionEditorCore.dll` | `ReplayEditor.vcxproj` (`editor/`) | ReShade's overlay and present callbacks | the editor's interface (ImGui through ReShade) and the video export |

The ASI and the editor never call each other. They share two named memory blocks per process:

- **The editor bridge** (`src/editor_protocol.h`, `src/editor_bridge.cpp`). The ASI publishes a `Snapshot` (transport
  state, the clip in memory, its camera markers, the gallery, targets) and pops `Command`s the editor queues (play,
  seek, add a marker, open a clip, export). Both sides check a magic number and a layout version and refuse a
  mismatch.
- **The export channel** (`src/export_channel.h`). During a video export the ASI runs a fixed-step export clock and
  writes one time stamp per tick; the core captures each presented frame, matches it to its stamp, accumulates the
  frames of the motion blur and queues finished frames to the encoder (`editor/render_export/`).

## Recording and playback

A recording is a set of tracks of timestamped packets, one track per kind of data, keyed by entity: create, delete
and attach events (with bone and offsets), transforms, skeleton poses, appearance, weapon state, ropes, doors, speech,
footsteps, shots, sound packets, effects, decals, fire, and world state (time, weather, sky, map pieces). The camera
markers are a separate track: editing the camera never touches the entity data.

Every file the mod writes goes under one base folder (`src/storage_paths.h`): the game folder when a test file can be
created, reopened and deleted there and in each data folder, else `Documents\Rockstar Games\Red Dead Redemption 2\Redemption Editor`,
else `%LOCALAPPDATA%\RedemptionEditor` (a Program Files install takes no writes from a game that is not elevated; a
write Windows redirects to the VirtualStore counts as refused). Reads go over the base, then the game folder, so
older clips still list and open, a name in both being the base's. `RedemptionEditor.ini` stays in the game folder;
the settings the editor changes then go to the base folder's copy, layered over it per key (`mod_identity.h`).

On disk a clip (`ReplayClips\NNN-Place.rpclip`, `src/clip_file.h`) is a header and one versioned section per track;
old clips are compressed in the background, losslessly (`src/clip_compress_worker.cpp`: the packed container of
`src/clip_pack.h`, pose and rope fields split into per-channel streams and LZMA blocks; older zstd clips still open
and are repacked). A project
(`ReplayProjects\*.rpproj`, `src/replay_project.h`) lists clips with their own trims and cameras.

Playback creates an inert **puppet** for each recorded entity and applies the packets at the **playhead**, the one
authoritative time for play, pause, scrub, frame steps and seeks (`src/replay_transport.h`). Engine state is recorded
and put back, not derived again from natives at playback time: skeleton poses are written into the puppets through
the engine's pose finalizer, and events (speech, sounds, effects) are replayed through the engine's own systems.

While a replay plays it owns the world (`src/world_theatre.h`, the support switch `WorldModel`): live people, vehicles
and story characters are removed, the game's scripts are paused, new spawns are denied and ambient emitters stopped;
on exit the latest save is reloaded. Each of these changes lasts only while the replay owns the world.

## Engine access ladder

Every feature uses the lowest rung that works, and climbs only when the rung below has failed with evidence:

1. **Natives** through Script Hook RDR2, on the script thread.
2. **Guarded reads** of engine fields whose layout was proven offline for the supported game build, inside SEH
   (`src/guarded_memory.h`), with a log line when a read faults.
3. **Guarded writes** of plain fields with a proven layout, only on objects the replay owns (its puppets, their
   weapons, the ropes it created).
4. **Calls to internal engine functions**, only after the function's first bytes match the expected signature at its
   known address, with the full argument list and SEH. A mismatch turns the feature off and logs it.
5. **Detours** (MinHook), with the same signature check. A detour forwards every call to the original and changes
   behaviour only for replay entities, or for the world while the replay owns it. Detours are installed once, removed
   on unload, and wait for calls in flight.

Never allowed: guessed addresses, byte patches without a signature check, engine state touched from the render thread
(the ReShade callbacks), and freeing engine memory through a deallocator whose contract is not proven. The engine
features a player may need to turn off have a support switch: not in the shipped ini, read when added by hand
([release/troubleshooting-keys.md](release/troubleshooting-keys.md)).

## Threads

- **Script thread** (Script Hook's fiber on the game's main thread): every native call, recording, playback, the
  transport and the free camera.
- **Engine threads in detours**: a detour never calls a native. It copies what it needs into a ring buffer that the
  script thread reads on its next tick (for example the pose finalizer feeding the pose capture).
- **Workers of the ASI**: the pose encoder and discovery workers, the clip save and clip read workers, background clip
  compression, the deferred log writer and the crash dump writer. They work on copies and never touch the engine.
- **ReShade's render thread**: the editor core draws its interface and runs the export capture there. It reads the
  shared snapshot and writes commands; it never touches engine state.
- **Workers of the core**: the encoder, audio capture and file finisher of the export, and thumbnail decoding.

Threads created by the mod or the core are named (`src/thread_names.h`), so a crash log or a minidump says whose
thread faulted.

## Builds

The public build reads only the keys of `src/ini_public.h`. The developer build (`REDEMPTION_DEV`, see
[building.md](building.md) and [release/ini-and-dev-build.md](release/ini-and-dev-build.md)) adds the research probes
and reads every key.
