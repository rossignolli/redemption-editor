# Animated shader variables (horse muscles)

Reverse engineering notes behind the replay of the horses' moving muscles: the
creature's shader vars component, who writes it and who reads it, and why the
replay writes the recorded values from a detour of the job's frame write node.
RDR2 build 1491.50, dump rdr2-1491.50-pid18760, plus the in-game measurement on
clip 1272 noted below.

## Where the values live (src/shader_vars_core.h)

```text
Where they live (build 1491.50, dump rdr2-1491.50-pid18760):
 - Every creature whose drawable has animated shader variables gets a
   shader vars component (its ctor sets its vtable and the type byte
   +0x10 = 9; built in two places, one for the vehicle cloth squash).
   Its slot 5 answers "do I have this DOF" for the
   tracks 0x11 (shaderSlideU), 0x12 (shaderSlideV) and 0x17
   (animatedNormalMaps): the muscle weights are DOFs of that component.
 - The component keeps its own frame: the DOF table (crFrameData) at +0x28
   (+0x28 table of 6-byte DOFs: u16 value offset, u16 id, u8 track, u8 type
   & 0xF; +0x54 count) and the values at +0x68 (crFrame:
   +0x68 buffer, +0x70 the frame data = component + 0x28, +0x9C
   u16 buffer bytes; a value of all ones is "not set", the buffer is filled
   with 0xFF at init).
 - The anim director's job writes it every animation update: it
   chains, after the pose finalizer node (its queue -> its job -> the
   pose finalizer), a frame write node (its queue -> the node) whose
   destination is the component's frame (lea rdx,[rax+68h]) and whose
   source is the motion tree's output: the node fills the destination with
   0xFF and copies every mapped DOF. The weights are what the horse's
   motion tree computes from its animation (clips and expressions); a
   puppet's motion tree plays nothing, so its weights are the rest values
   or "not set", whatever the replay writes into its skeleton.
 - The ped shader effect reads them in its update (its vtable's slot
   24, update(effect, ped): ped type 4 only) through its reader(effect,
   ped): ped -> [+0xA8] -> [+0x48] creature -> the component find (by
   its type: index byte [creature + 0x74 + type], [creature][index]) ->
   the bindings at +0x18 (16-byte rows: hash, value offset, id, track,
   type; u16 count at +0x20) -> the value at [component + 0x68] + offset,
   "not set" read as 0, clamped and packed into the effect's instance.
```

## What the replay does

What the replay does (it records the engine's results per entity and
per frame, not the inputs the engine derived them from): the
recording keeps each recorded horse's component values, about 30 times a
second (section SHVR), and the playback writes the recorded values into
the puppet's component buffer at the end of the engine's own write (a
detour of the job's frame write node, shader_vars_hook.h): every
reader after it, the shader included, sees them until the next update.

## Engine reads (src/shader_vars_hook.h)

```text
Reads (guarded; every function the layout comes from found by its
signature once a session, shader_vars_hook.cpp):
  ped -> u64 [ped + 0xA8] -> u64 [+0x48] creature        (the shader reader)
  index = u8 [creature + 0x74 + type], type = u8 [the type byte global],
  component = u64 [u64 [creature] + index * 8] (0xFF: none) (the component find)
  component: the vtable its ctor stores, u8 +0x10 = 9 (the ctor);
  DOF table u64 +0x28, count u32 +0x54; values u64 +0x68 with the frame
  data u64 +0x70 = component + 0x28 and the buffer bytes u16 +0x9C
  (the frame's readers).
```

## The write: first try and the current detour

Write (rung 5, MuscleReplay). First try: a detour of
the ped shader effect's reader that wrote the values before it
read them. Measured in game (clip 1272): the reader runs for a puppet in
about 1 frame in 15 (hits 40012 of 602341 published puppet ticks) and the
puppet's buffer never moved (puppet_step_max 0): the animation job fills
the buffer again every update (0xFF, then the motion tree's rest values),
and what the reader packed was not what the renderer kept.
Now: a detour of the job's frame write node (node), the only writer of
the buffer (the creature update job queues it through the queue function
with lea rdx,[component+68h]: node +0x28 = the buffer, +0x50 its bytes). After
the original copies the motion tree's values, a node whose destination is
a published puppet's buffer gets the recorded values on top. Every later
reader (the shader effect, the vehicle readers, the script thread's
evidence) sees them until the next update writes again. The reader
detour stays as an observer only: at each shader read of a published
puppet it compares the buffer with the published values (shader_reads,
shader_lanes, shader_matched).
