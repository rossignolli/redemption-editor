# Ped and horse puppets

Reverse engineering notes behind how the replay keeps its ped and horse puppets drawn, animated and still (RDR2 build 1491.50, offline dump unless stated). The code keeps the contract and a link here.

## Puppet LOD and skipped animation updates (src/puppet_lod.h)

The engine can draw a puppet at a low LOD (e.g. a hat as a white low polygon
block above the head) and, in those frames, the pose finalizer does not run
for it (the replay's pose is not written: POSE_JERK causes=not_written), so
the puppet blinks and jumps. Rung 1, on the replay's own puppets:

```text
 - SET_PED_LOD_MULTIPLIER (0xDC2C5C242AAC342B; handler -> core,
   writes the multiplier and the entity's LOD distance
   multiplier x 100) high, so the puppet keeps its high LOD far away;
 - SET_ENTITY_ALWAYS_PRERENDER (0xACAD101E1FB66689; handler -> core,
   the global list): the pre-render (skeleton matrices for the
   draw) runs even off screen;
 - FORCE_PED_AI_AND_ANIMATION_UPDATE (0x2208438012482A1A; handler ->
   core, reset flags of the ped's update) on a tick that
   finds no pose write since the last one (the frame in between rendered
   an old pose).
Undone when the puppet is deleted (prerender off before the entity goes,
so the global list never keeps it; multiplier back to 1).
```

No native tells the draw LOD chosen for a frame. A skipped finalizer is the
engine skipping the puppet's animation update (the finalizer is reached only
from the motion tree: no update, no call), not the draw LOD: it happens at
2 m from the camera too, far more often when the hidden player stays far
from the scene, and FORCE_PED_AI_AND_ANIMATION_UPDATE on a stale tick does
not make the next tick write. As a replay writes the ped's skeleton every
frame whatever the animation passes do, a puppet the engine skipped gets the
published pose from the script thread every tick for kScriptPoseHoldTicks
after its last skipped tick (ApplyOverrideNow, the guarded write already
used at every arm and every tick for vehicles). Two counts: own writes (the
finalizer's: the engine updated the puppet) and any writes (own plus the
script's). A stale tick is a tick without an own write; an episode
(puppet_low_lod: the puppet shown with an old pose, on screen, for
kLowLodRunTicks ticks in a row while the playhead moved; one
POSE_V2_PUPPET_LOW_LOD line per episode) needs a run of ticks without any
write.

## Collision filter of a walking puppet (src/puppet_collision.h)

Collision filter of a walking ped or horse puppet. The puppet must stay an
active physics body (bushes react only to an active body), but then the
engine's physics pushes it off the recorded track. So the puppet keeps its
active instance for the plants and collides with nothing else: its physics
instance's include flags are restricted to the vegetation and query
classes, so no pair with the map, peds, horses, vehicles or objects is ever
formed (a pair needs both sides to include each other). The end-of-frame
write (mount_placement.h) covers whatever still moves it.

Engine facts (build 1491.50):

```text
- entity +0x38 = the current physics instance (phInst), +0x08 of it the
  archetype: _SET_PED_USE_HORSE_MAP_COLLISION's body reads
  [ped+0x38] and [inst+0x08].
- inst +0xC0 = type flags (what it is), +0xC8 = include flags (what it
  collides with): the same function writes include and type
  (0x200 ped capsule, 0x800 horse capsule: `and eax, 0x600;
  add rax, 0x200`), with the instance in the level and no level call;
  the level insert copies archetype +0x28/+0x30 into them.
- pair filter: (A.include & B.type) != 0 and (A.type &
  B.include) != 0, on per-frame copies of the two fields.
- class bits from the script shape test flag translation and the
  Foliage Detector's include 0x80000: map 2 and 3, vehicles 6,
  7, 25, peds 9, horses 11, ragdolls 10 and 12, objects 13, foliage 19.
  Env cloth 14 and plant 15 (inferred, not proven), query classes 21 to
  24 (24 proven: the script test type).
```

## The animate as if visible byte (src/anim_visible.h)

Every frame the pre-render builder asks the visibility test about each ped
it pre-renders (engine_sigs.h kComponentRead). The test decides whether the
ped is visible for the animation: one of its visibility stamps (+0x102,
+0x103, +0x104) equals the frame tag, or the ped graphics component's byte
+0x4A8 is set (kAnimVisibleTest), or the mount or attach parent is visible,
or a global animscene state is on. It then hands the anim director the LOD
(component+0x4A0) and visible (anim_lod_hook.h), which stores the creature
component's effective LOD (+0xAF) = visible ? lod : 3. A change of it
reaches the expressions component: an expression stays on only while its
LOD range [+0x81, +0x82] holds the LOD. Off screen the LOD is 3 and every
expression that stops below 3 (face rig, hat fit, brim springs, manes) is
off: its rows stay unwritten (0xFFFFFFFF lanes) and the finalizer takes the
bind pose (measured: +530 to +810 rows per human, +661 per horse, +280 per
dog).

The byte is written only by the component's constructor (0) and by the
pedshot slot setter (1 on assign, 0 on release, together with
always-prerender and a LOD multiplier of 100): the engine's recipe for a ped
fully animated off screen. SET_PED_LOD_MULTIPLIER only feeds the requested
LOD (component+0x49C -> +0x4A0): it fixes distance, not visibility;
SET_ENTITY_ALWAYS_PRERENDER only puts the ped in the pre-render set (the
test runs, visible stays false).

Layout (in the bytes of kComponentRead and kVisibilityTest):

```text
  entity type byte +0x30 == 4 (ped);
  slot = (u32 [ped+0x9C] & 0x1FFFF) - u32 [base index];
  row = u64 [table] + slot * 0x148;
  component = u64 [row+0x98] & ~1 (0: none);
  check: (u64 [component+8] & ~0xF) == ped.
```

## Metaped expressions layout (src/metaped_state.h)

Expressions are read from the engine's structure, not asked one by one
(build 1491.50, the getter _GET_CHAR_EXPRESSION from its handler to its
core):

```text
  creature  = [entry & ~1 + 0x28], entry = qword [table + (idx - base) * 16],
              idx = [ped + 0x9C] & 0x1FFFF (the creature lookup, its table
              and base index globals)
  container = [creature + 0x18 + sel * 8], sel the first byte of
              creature + 0x104 + bufferIndex * 4 + 0..3 that is not 0x14
              (the frame container, bufferIndex a global byte)
  frame     = container + 0x58 (the expression frame): [frame] the value buffer,
              [frame + 8] the frame data: [data] the DOF table, dword
              [data + 0x2C] its count (the DOF find), 6 bytes a DOF:
              +0 u16 value offset, +2 u16 id, +4 u8 track, sorted by
              (track << 16 | id); expressions are track 0x40 (the
              expression frame); a value dword 0xFFFFFFFF is "not set"
              (the frame value).
```

Every id of track 0x40 is one _GET_CHAR_EXPRESSION / _SET_CHAR_EXPRESSION
index (the setter's core queues the (u16 id, float) pair on
the dressing object [[ped + 0x28] + 0x750] + 0xB20).

## The player puppet is never hidden (src/player_hold.h)

QA 224 (a tester: "Arthur becomes invisible, mostly when the recording stops
and starts another"): the clip's player record had lost its first 68 s. A
pose track used to drop its oldest chunks past a 32 MiB budget of its own
(pose_stream.h StreamConfig::byteBudget); Arthur's record, 1050 bones on
the move, is the heaviest of a scene and reached it first (33.36 MB kept,
34 two-second chunks dropped, the horse beside him at 29.6 MB). The
playback hid every body before its record's first frame, so he was missing
for the whole start while his horse rode on alone. The capture now leaves
the bytes to the recorder's own budgets (pose_capture_rules.h
kCaptureTrackByteBudget); for clips already recorded, and any hole a
recording may still leave, the player's puppet holds the nearest recorded
pose (the track's sample clamps to its first and last frame) instead of
being hidden.

When his first frame has him on a horse (QA 224: in the saddle of 0:1 at
68 s, the horse ridden since 0 s), the held pose rides that horse: before his
first frame he keeps the rider-in-mount offset of that frame on the mount's
recorded transform (Carry), instead of standing frozen where the record
begins, hundreds of metres from the camera.

## Horse muscle detail, animated normal maps (src/muscle_watch.h)

The engine's animation frame of a creature holds more than bones: every
DOF is (track, id) and the track names are a table in .rdata (the names,
"animatedNormalMaps" among them; the parallel id bytes give
boneTranslation 0, boneRotation 1, boneScale 2, visibility 4, blendShape
0x15, animatedNormalMaps 0x17).
The muscles that move under a horse's skin are animated normal maps:
blend weights on track 0x17 that the expressions compute each frame. The
replay records and writes only the bone rows (the pose finalizer's
output); track 0x17 of a puppet is whatever its own expressions produced,
and an expression runs only while the director's effective LOD is in its
range (anim_visible.h, anim_lod_hook.h). The recording forces LOD 0 and
visible on every recorded ped (RecordPedAnimLod0); the playback did not.

## The speed the ped audio reads (src/ped_speed_core.h)

Each ped audio holder owns a PED_VARIABLES block (ped_audio_state.h, part
b) that the sounds of that ped read by name. UpdatePedVariables
writes its "speed" (0x8BAE9C31) every audio update from the movement
component (row +0x88 +0x5DC; the component writes it in its speed update
from a smoothed |physical velocity - ground velocity|) through the
block setter (mov edx,8BAE9C31h; mov rcx,rbx; call the setter).
The name appears nowhere else in the executable: only the sound metadata
reads it. A puppet is frozen and moved by the replay, so its component
keeps 0: every puppet's sounds would read speed 0 where the recording read
the ped's speed.

As a replay editor usually does it (each ped's velocity in its packet, given
to the ped's audio entity at playback), the recording keeps each recorded
ped's (and horse's) block speed 4 times a second (section PSPD), and the
playback writes it into the puppet's block right after the engine's own
update (PedSpeedReplay, a detour on it that forwards every call). A
clip without the section gives the puppet the speed of its recorded motion
(its displacement over clip time).

