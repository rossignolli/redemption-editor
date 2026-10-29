# Support switches (hidden troubleshooting keys)

For support, not for players. The player-facing page ([../configuration.md](../configuration.md)) only says that
advanced switches exist and that we will tell the player when one is needed.

## What they are

51 engine safety switches. Each one turns on code that changes or calls the game's engine (a detour, an engine
function or a memory write). All are **on** by default. Since release 1.0 they are **not in the shipped ini**
(`src/ini_public.h` `kShippedIni`, `dist/RedemptionEditor.ini`), but the public build **still reads them**: they are in
`kPublicReadKeys` and listed with their defaults in `kSupportSwitches`. `tools/check_public_build.py` checks that
this page lists every one of them.

## How to give one to a player

1. Ask for the report ZIP (Shift+F1) first, and find the area in the log or the crash stack.
2. Give **one** line at a time, to add under `[RedemptionEditor]` in `RedemptionEditor.ini` (game folder), for
   example:

   ```ini
   [RedemptionEditor]
   DecalGate=0
   ```

   The mod also reads it under `[Troubleshooting]` or the old `[ReplayPoC]` (`src/mod_identity.h` `SectionFor`; a
   value under `[RedemptionEditor]` wins), but only `[RedemptionEditor]` shows up in the log's `CONFIG_INI,...,keys=`
   line, so you can confirm the player set it.
3. The game must be restarted: the ini is read at start.
4. Ask for a new report with the switch off. If the problem stays, have the player remove the line again: a switch
   left off only makes the replay worse.
5. A switch that fixes a problem for players is a bug to fix in code, not a setting to leave off.

## The switches

"Off" is the value to give (`0`, or `off` for `WorldModel`). "When to suggest it" is the symptom or crash that
points at it.

### World ownership (the "closed theatre")

| Key | Default | What turning it off disables | When to suggest it |
|---|---|---|---|
| `WorldModel` | `gta` | The editor taking the world over while a replay plays (clears people and traffic, pauses the game's scripts, reloads the latest save on exit). `off`: the world keeps living behind the replay and no save is reloaded. | A crash or hang on entering or leaving playback, a broken save reload on exit, or a mission/story state that breaks after a replay. Last resort: the replay will show live people. |
| `TheatreHooks` | `31` | The engine detours used while a replay owns the world (bit 1 spawn funnel, 2 mix scene start, 4 speech state 3 rules, 8 speech state 3 failure code, 16 mix scene stop). `0`: people can spawn into the replay; voices and scene sounds can be wrong. | A crash whose stack is in the spawn path, the audio mixer scenes or speech while a replay plays. A single bit can be cleared (for example `29` drops the mix scene start). |
| `TheatrePerschar` | `1` | Removing the story characters while a replay plays. `0`: they can stand in the replay. | A crash or a story character that breaks right when a replay starts. |
| `TheatreVehicles` | `1` | Removing the vehicles while a replay plays. `0`: live wagons and boats can stay in the scene. | A crash when a replay starts near wagons, trains or boats. |
| `TheatrePeds` | `1` | Removing the people while a replay plays. `0`: live people can stay in the scene. | A crash when a replay starts in a busy place (Saint Denis, camp). |
| `TheatrePauseScripts` | `1` | Pausing the game's scripts while a replay plays. `0`: scripts can move or talk over the replay. | A mission or another script mod that breaks or hangs after a replay. |
| `TheatreDenySpawn` | `1` | Stopping new people and animals from spawning while a replay plays. `0`: they can walk in. | A crash in spawning during playback, or a world that stays empty after the replay ends. |
| `TheatreEmitters` | `1` | Stopping the world's ambient sound emitters while a replay plays. `0`: live ambient sounds mix in. | Ambient sound that stays muted after a replay, or a crash in the ambient sound emitters. |

### Sound

| Key | Default | What turning it off disables | When to suggest it |
|---|---|---|---|
| `SoundGeneric` | `1` | Recording and replaying every sound effect as packets (detours on the game's sound ports). `0`: only speech, steps, shots and melee are replayed. | A crash whose stack is in the game's audio while recording or playing, or loud, stuck or wrong sound effects in replays. The first sound switch to try. |
| `CategoryStateReplay` | `1` | Putting the recorded sound category volumes back. `0`: the mix follows the live game. | Whole categories (music, ambience, speech) too loud, too quiet or silent in a replay. |
| `SpeechReplayRules` | `1` | Letting the replay's speech lines through the game's speech rules. `0`: some lines are dropped. | A crash in speech playback, or speech that plays when it should not. |
| `SpeechDuckReplay` | `1` | Ducking the world's sound under replayed conversations as the game did. `0`: no duck. | The world stays quiet (ducked) after a conversation in a replay, or after the replay ends. |
| `SpeechConversationLive` | `1` | Replaying human lines with their recorded sound set and duck. `0`: lines can sound flat or too loud. | Wrong or garbled voices in conversations, or a crash while a conversation replays. |
| `PuppetOwnSpeech` | `1` | Refusing the puppets' own speech. `0`: puppets can say lines that were never recorded. | Recorded lines missing in a replay (the refusal is catching them). |
| `RefusePuppetOwnPersistent` | `1` | Refusing the puppets' own looping sounds. `0`: sounds that were never recorded can play. | Recorded looping sounds (breathing, tack, vehicles) missing in a replay. |
| `ListenerMicReplay` | `1` | Putting the recorded microphone back when the replay uses the game camera. `0`: the sound can come from another place. | Sound that seems to come from the wrong place or fades out with the game camera. |
| `WindReplay` | `1` | Keeping the wind sound alive in a replay far from the player. `0`: the wind can freeze. | A wind sound that is stuck, loops or is too loud in a replay. |
| `ShotEnvGroupReplay` | `1` | Weapon sounds taking the holder's sound space. `0`: shots can sound outdoors indoors. | Shots that sound wrong (wrong reverb, missing) in a replay, or a crash on shots. |

### Steps and movement sound

| Key | Default | What turning it off disables | When to suggest it |
|---|---|---|---|
| `FootImpactReplay` | `1` | The recorded step height on hoof and foot sounds. `0`: steps can sound too loud. | Wrong or missing steps; try the foot switches one at a time. |
| `FootLayerReplay` | `1` | The recorded ground layers on steps. `0`: steps can sound like another ground. | Steps that sound like the wrong ground, or a crash in foot sounds. |
| `FootWetnessReplay` | `1` | The recorded wetness on steps. `0`: dry steps in the rain. | Wet steps where the ground is dry, or a crash in foot sounds. |
| `FootMovementReplay` | `1` | The recorded step volume. `0`: steps can be louder or softer. | Steps that are too loud or silent. |
| `FootEventReplay` | `1` | Replaying the secondary steps (a hoof lifting or landing). `0`: gallops sound thinner. | Doubled or machine-gun hoof sounds at a gallop. |
| `PuppetAudioLodRecorded` | `1` | The puppets' sound detail as recorded. `0`: puppet steps can be quieter. | Puppet steps that are too loud or doubled. |
| `MotionSpeedReplay` | `1` | The puppets' movement speed for their sounds. `0`: gallops can sound like a walk. | Movement sounds that do not match the speed (a walk sounding like a gallop). |
| `PedSpeedReplay` | `1` | The recorded speed of people and horses for their sounds. `0`: hooves can be silent. | Same as `MotionSpeedReplay`, when that one did not help. |
| `EntityQueryRecord` | `1` | Recording the engine's answers about mounts and riders. `0`: saddle and tack sounds can be lost. | A crash or stutter while **recording** on horseback. |
| `EntityQueryReplay` | `1` | Giving those recorded answers back to the puppets. `0`: saddle and tack sounds can be lost. | A crash while a replay with riders plays. |
| `EntityQueryRelation` | `1` | A puppet rider's sounds following its replayed horse. `0`: tack sounds can be lost. | Tack or saddle sounds coming from the wrong horse or place. |

### Vehicles, weapons and objects

| Key | Default | What turning it off disables | When to suggest it |
|---|---|---|---|
| `ReinsHoldGuard` | `1` | Holding a puppet wagon's reins as recorded. `0`: the reins can snap back to the wagon. | A crash or a glitch on wagon reins in a replay. |
| `VehicleDoorWrite` | `1` | Writing the recorded door positions on puppet wagons and train cars. `0`: doors can stay shut. | A crash or a broken train car or wagon when a replay with doors plays. |
| `WeaponWearWrite` | `1` | Writing the recorded rust and wear on replayed weapons. `0`: weapons look clean. | A crash when replayed weapons appear, or a weapon with a wrong look. |
| `WeaponPassPose` | `1` | Playing a weapon's own moving parts (lever, cylinder, hammer). `0`: they stay still. | Weapon parts in a broken pose, or a crash on weapons in a replay. |
| `WeaponFxIntent` | `1` | Recording muzzle flashes and shells as the weapon asked them. `0`: some far or off-screen flashes are lost. | Doubled or misplaced muzzle flashes or shells. |
| `DecalGate` | `1` | Letting the game and the replay add blood and bullet decals one at a time. `0`: **a known crash can return**. | Rarely: only when decals (blood, bullet holes) go missing or the decal code is in a new crash stack. |
| `RenderTargetRecord` | `1` | Recording the camp ledger and other in-world screens. `0`: they are blank in the replay. | A crash or stutter while recording near the camp ledger, posters or other in-world screens. |
| `RenderTargetPlayback` | `1` | Showing the recorded in-world screens in the replay. `0`: they are blank. | A crash or garbled screens when such a replay plays. |
| `ObjectLook` | `1` | Putting the recorded textures and tints back on letters, posters and props. `0`: default look. | Props with wrong textures or tints, or a crash when props appear in a replay. |
| `FireCharCreate` | `1` | Putting the recorded burn state on burned people. `0`: burned bodies look clean. | A crash in a replay with burned people. |
| `ObjectBurnLevel` | `1` | Putting the recorded burn level on burned objects. `0`: they look unburned. | A crash in a replay with burned objects. |

### People and puppets

| Key | Default | What turning it off disables | When to suggest it |
|---|---|---|---|
| `RecordPedAnimVisible` | `1` | Recorded people and animals animating as if seen up close (hats, faces). `0`: off-screen detail can be wrong. | Frame drops while **recording** in busy places. |
| `RecordPedAnimLod0` | `1` | Recorded faces animating at full detail far from the camera. `0`: far faces can freeze. | Frame drops while recording, when `RecordPedAnimVisible=0` was not enough. |
| `PuppetScriptPose` | `1` | Writing a puppet's pose when the engine stops animating it. `0`: far puppets can freeze. | Puppets popping or snapping into a pose, or a crash in posing. |
| `FrameEnd` | `1` | Placing puppets at the end of each frame. `0`: riders and wagons can jitter. | A crash or stutter in the per-frame placement; expect jitter with it off. |
| `PhysicsFilter` | `1` | Puppets colliding only with plants. `0`: puppets can push each other. | Puppets passing through the world, or a physics crash. |

### Effects and sky

| Key | Default | What turning it off disables | When to suggest it |
|---|---|---|---|
| `FxPuppetNoWarp` | `1` | Keeping effects on moving puppets from jumping. `0`: smoke and dust can flicker on riders. | Effects on riders that lag or vanish, or a crash in particle effects. |
| `FxPuppetVelocity` | `1` | Effects on puppets following their motion. `0`: dust and smoke stay behind. | Effects stretched or flying off puppets. |
| `FxPauseFreeze` | `1` | Freezing particles while a replay is paused. `0`: particles keep flowing. | Particles that stay frozen after unpausing, or a crash on pause. |
| `FxSettingsReplay` | `1` | Putting the recorded effect settings back. `0`: effects can look different. | Effects with wrong colour or size, or a crash when effects start in a replay. |
| `FxRelocateClearStill` | `1` | Clearing the jump mark of effects on still props. `0`: their effects can flicker. | Effects on still props (lamps, fires) in the wrong place. |
| `MoonReplay` | `1` | Playing back the moon's position. `0`: the moon stays where the live game has it. | A wrong sky or moon at night in a replay. |
