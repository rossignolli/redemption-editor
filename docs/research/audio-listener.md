# Audio listener and the distance stage

Engine notes behind the recording and replay of the game's volume listener,
the mix probe that compares the replay's mix with the recording's, and the
gunshot start delay (RDR2 build 1491.50, offline dump unless noted).

## Volume listener (src/game_listener_core.h)

With the same game camera the replay was 7 dB lower without it: the ped
audio distance metric (+0x4640, the minimum squared distance to the
listeners' volume points) gives 1.0 m for the player's own lines live
(camera 2 to 10 m away) and the camera distance in the replay. The live
listener sits near the player, the replay's at its script camera.

Engine layout (the code sites below hold every offset):
- the audio environment singleton (kEnvironment, which the ped audio update
  hands to the listener test) holds two listener descriptions of 0x310
  bytes at +0x20 (current and next of a microphone crossfade; current index
  byte +0x784);
- each description has volume points: count dword +0x2B0, point j position
  at +0x1D0 + j*0x90 (the volume metric takes the minimum squared distance
  over them, the point slot getter returns the slot);
- the microphone settings are chosen per frame from the dominant camera
  (the microphone chooser: the override hash +0x780 a camera's metadata
  sets, else a map from the camera's metadata name), and each volume
  microphone places its point on an entity (type 2 the camera's attach
  entity, 3/4/8 globals that hold the player, 0 the camera frame) plus
  micLength toward or away from the camera;
- the engine's own volume listener override: bit 8 (0x100) of +0x650 with
  the matrix +0x6B0..+0x6E0 (position +0x6E0) replaces the computed point
  (in the volume builder); the animated scene handlers set it, the volume
  reset clears the matrix once the bit is clear.

The replay records the live volume point (absolute position) and, while it
renders the recorded game camera, puts it back through that override, the
way the engine's scenes do (a write to a world system: only while the
replay owns the world, cleared on exit).

## Microphone selection (src/game_listener_core.h)

The microphone:
- per point j the slot is desc+0x190+j*0x90 (point_slot); the volume
  builder writes its weight 1/count at slot+0x50 (slot_weight_write) and
  the microphone metadata's float +0xC at slot+0x70 (slot_f70_write);
- the microphone in use is the metadata pointer at desc+0x300
  (mic_in_use_cmp compares [env+0x320+0x310*cur]);
- the chooser takes the override hash +0x780 with the crossfade +0x778
  (mic_crossfade_read); without the override it asks the camera map
  (writes only its out dword and its frame; callees are getters and a
  binary search), whose answer is a metadata reference looked up by the
  reference lookup (not a hash), then a second map at env+0x10, else the
  default hash 0x2A7C18A2 by the metadata lookup (the microphone table,
  type 0xA9); the replay records the hash of the metadata in use
  (desc+0x300) through the table's (offset, hash) entries;
- the engine writes +0x780 only in two animated scene handlers (a
  scene's camera metadata, crossfade +0x778 first) and zeroes it in two
  others (rip relative stores; the only other access to +0x780 in the
  code that names the environment is mic_override_read).

## Mix probe (src/mix_probe_core.h)

Why the replay can sound weaker than live ("no base", gunshots and horse
hooves weaker, most in gunfights). Six suspects, each answered from what the
probe records in both passes (record and play):
1. listener: the distance from the sound's owner to the rendered camera
   (and to the live player ped, and the engine's own distance metric of
   the owner's ped audio) differs between the passes;
2. player treatment: the engine treats the player's sounds apart (the
   player bit of the component row slot +0xB8, byte +0x40; the weapon
   audio predicate and the mount predicate (audio_observer.cpp
   kPlayerAudioPredicate, kMountAudioPredicate), which the replay's
   detours answer for the player's puppet and its horse; the movement
   volume reads the bit directly, not covered);
3. variant: the engine picks another sound for the same owner and event
   (the NPC weapon sound chosen by distance);
4. audio LOD: the ped audio holder's LOD (+0x45E8) drops components
   farther from the listener;
5. music: the game's score plays live and is stopped in the replay
   (MusicStop=1, as a replay editor does);
6. count: the replay makes far fewer sounds of a role and kind (hooves
   never created with the free camera): the five above only look at sounds
   that exist.

Variants are matched by sound set (the ref's low dword), the ped steps
queued through S03 are steps (RequestFamilies), the LOD verdict looks both
ways, refused requests stay out of the counts and rates, and in the replay a
weapon's holder comes from the clip's WeaponPoint attachment (mix_probe.cpp).

## Gunshot start delay (src/shot_audio_delay.h)

The chain followed here, all of it on the engine's own objects:

```text
  fire event --(the fire queue and its consumer)--> gunshot sound
  created on the weapon's audio (weapon +0x100; the census sees the
  creator, row.audio and row.sound) --(the audio thread prepares it, the
  wave bank loaded or not)--> play state "playing".
```

The top-level sound's play state is the two top bits of byte +0xF2: the
requested-settings processor reads it (movzx esi,[rbx+0F2h]; shr esi,6)
and switches on it; the sound's prepare writes 1, its play writes 2
(beside the start time at +0x8C) after the prepare answered 1, and its
stop writes 3. Read as 0 dormant, 1 preparing, 2 playing, 3 waiting to be
deleted.

The gunshot's init params carry no predelay (+0xEC) and no start offset
(+0xE8): the gunshot's play function builds them at [rbp] (the params
constructor) and then writes +0xA4, +0xA8, +0xB0, +0xC8, +0xD0, +0xE0
(environment group), +0x104, +0x138 and +0x13A, never +0xE8 or +0xEC, and
recorded gunshot packets list neither among the non-default dwords. So the
engine does not delay a distant gunshot by distance at its creation; the
travel time printed (distance / 343 m/s) is a reference only.
