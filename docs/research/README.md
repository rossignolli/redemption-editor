# Research notes

Reverse engineering notes behind the code: how engine structures and functions were found,
the offsets and instruction addresses that prove them, failed attempts and test results.
The source files keep only the contract, the reason for non-obvious choices and the
evidence class, with a `// Research: docs/research/<topic>.md` line pointing here.

Unless a note says otherwise, addresses are RVAs of RDR2 build 1491.50.

## Audio

| Note | Subject | Referenced from |
|---|---|---|
| [audio-voice-volume.md](audio-voice-volume.md) | Audio voice volume and the mixing chain | `src/volume_probe.h`, `src/volume_probe_core.h` |
| [audio-mix-scenes.md](audio-mix-scenes.md) | Audio mixer scenes, mix groups and sound categories | `src/category_state_core.h`, `src/mix_scene_core.h`, `src/mix_scene_pool_core.h` |
| [audio-listener.md](audio-listener.md) | Audio listener and the distance stage | `src/game_listener_core.h`, `src/mix_probe_core.h`, `src/shot_audio_delay.h` |
| [audio-generic-sounds.md](audio-generic-sounds.md) | Generic sounds: recording, playback and ownership | `src/sound_census_core.h`, `src/sound_owner_table.h`, `src/vehicle_sound_rule.h` |
| [audio-speech-params.md](audio-speech-params.md) | Speech request parameters of the direct line entry | `src/audio_observer.h` |
| [entities-sound-query.md](entities-sound-query.md) | Owner state queries of dynamic entity sounds | `src/entity_query.h` |

## Speech, ped audio and footsteps

| Note | Subject | Referenced from |
|---|---|---|
| [speech-line-request.md](speech-line-request.md) | Speech line requests | `src/speech_level_core.h`, `src/speech_player_route.h`, `src/speech_replay_rules.h`, `src/speech_request.h`, `src/speech_script_path.h` |
| [speech-duck-and-slots.md](speech-duck-and-slots.md) | Speech duck, slots and conversations | `src/animal_bank.h`, `src/speech_conversation.h`, `src/speech_duck_core.h`, `src/speech_slots.h` |
| [ped-audio-state.md](ped-audio-state.md) | Ped audio state | `src/horse_audio_evidence.h`, `src/ped_audio_state.h` |
| [footsteps.md](footsteps.md) | Footsteps | `src/foot_event_core.h`, `src/foot_material.h` |

## Peds, poses, horse tack and hats

| Note | Subject | Referenced from |
|---|---|---|
| [peds-puppets.md](peds-puppets.md) | Ped and horse puppets | `src/anim_visible.h`, `src/metaped_reader.h`, `src/metaped_state.h`, `src/muscle_watch.h`, `src/ped_speed_core.h`, `src/player_hold.h`, `src/puppet_collision.h`, `src/puppet_lod.h` |
| [peds-weapon-straps.md](peds-weapon-straps.md) | Weapon straps and weapon components on puppets | `src/strap_state_core.h`, `src/weapon_order_core.h` |
| [pose-bind-and-finalizer.md](pose-bind-and-finalizer.md) | Bind pose, the pose finalizer and T-pose detection | `src/anim_lod_hook.h`, `src/pose_skeleton_layout.h`, `src/prefix_hold.h`, `src/tpose_check.h` |
| [pose-frozen.md](pose-frozen.md) | Frozen poses off camera | `src/frozen_pose_check.h` |
| [horse-tack-mount.md](horse-tack-mount.md) | Mount, rider and horse tack on replay puppets | `src/mount_placement.h`, `src/reins_guard.h` |
| [hats-look-and-chain.md](hats-look-and-chain.md) | Hats: the loose hat look and the hat chain | `src/hat_chain.h`, `src/hat_look.h` |

## World, weather, shaders and effects

| Note | Subject | Referenced from |
|---|---|---|
| [weather-wind-sky-timecycle.md](weather-wind-sky-timecycle.md) | Weather, wind, moon and timecycle | `src/moon_track.h`, `src/timecycle_state.h`, `src/timecycle_track.h`, `src/weather_state.h`, `src/wind_replay_core.h` |
| [shaders-muscle-vars.md](shaders-muscle-vars.md) | Animated shader variables (horse muscles) | `src/shader_vars_core.h`, `src/shader_vars_hook.h` |
| [fx-fires-decals-effects.md](fx-fires-decals-effects.md) | Fires, decals and effects | `src/burn_level.h`, `src/fire_initial.h`, `src/fx_motion_rule.h`, `src/script_fx_loop.h`, `src/weapon_fx_intent_core.h` |
| [world-drawable-store-crash.md](world-drawable-store-crash.md) | DrawableStore crash probe | `src/drawstore_probe_core.h` |

## Entities, objects, vehicles and camera

| Note | Subject | Referenced from |
|---|---|---|
| [entities-pools.md](entities-pools.md) | Entity pools and the per-frame entity cache | `src/entity_cache.h`, `src/entity_pools.h` |
| [objects-map-owner.md](objects-map-owner.md) | Map data objects by their owner | `src/object_check.h` |
| [objects-look.md](objects-look.md) | Object look: document text and tint | `src/object_look_core.h` |
| [vehicles-doors.md](vehicles-doors.md) | Vehicle and train car doors | `src/vehicle_doors_engine.h` |
| [vehicles-pieces.md](vehicles-pieces.md) | Vehicle pieces and wagon wheels | `src/vehicle_pieces.h` |
| [camera-cut.md](camera-cut.md) | Camera cuts in the replay | `src/camera_cut.h` |

## Recording, export and editor

| Note | Subject | Referenced from |
|---|---|---|
| [recording-window.md](recording-window.md) | Recording window: how each track is cut | `src/recording_window.h` |
| [recording-capture-probe.md](recording-capture-probe.md) | Capture probe: fragment and lamp breaks | `src/capture_probe.cpp` |
| [recording-render-targets.md](recording-render-targets.md) | Named render targets and UI apps: engine model | `src/render_target_core.h` |
| [export-timer.md](export-timer.md) | Export timer: locking the game clock to the export clock | `src/export_timer.h` |
| [editor-ui-art.md](editor-ui-art.md) | Editor UI art files | `editor/ui_art.h` |
