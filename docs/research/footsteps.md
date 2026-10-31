# Footsteps

Reverse engineering notes behind the replay of footstep and hoof sounds: the
secondary step queue of the foot audio and the ground material a step reads.
Build 1491.50, offline dump unless a note says otherwise. The code keeps the
contract and links here.

## Secondary steps: the engine side (src/foot_event_core.h)

Engine (the code sites below hold the layout):
  - every update of the movement component C (row +0x88; its per-foot loop)
    measures h, the height of the foot over its ground plane (stored at C
    +0x164 + 0x90 * foot), and calls the lift/land detector (the generator,
    kGenerator) with (C, h, foot); there h is compared with [C +0x4F0] +
    [C +0x550] * [C +0x5E8]: a foot down ([C +0x15C + 0x90 * foot] < 6) that
    rose above it lifted, a foot up that came under it landed. The event
    type comes from the gait table (by the gait C +0x630: 3, 4, 5, 6 give
    0..3 lifting and 6..9 landing; a standing quadruped 4/10, the leg mask
    5/11, gait 9 0/6, other gaits none) and goes to the second queue's entry
    (foot audio, type, foot, spread, direct; kEnqueue) at kGeneratorCall;
  - the entry refuses a foot audio with +0x5A7 set, type -1 and a foot of
    12 or more; for a quadruped with direct 0 it parks the event in a
    delayed table (+0xD8, 16 entries of 12 bytes, the delays of the hoof
    slots +0x3A0), which the movement update drains at its start by calling
    the entry again with direct 1 (kDrainCall); otherwise it queues {type,
    foot} at +0xB4 + 8 * n with the count at +0xD4 (at most 4; the store,
    kEnqueueStore, is the only increment);
  - the movement sounds (inside the movement update, after the main steps)
    play that queue through the secondary step, which plays feet 0..3 only:
    the type picks a field of the soundset of the foot's slot (foot audio
    +0x240 + 0x40 * foot, the same container the main step uses) by a hash
    table, created at kSecondaryStepCall (the callers[2] of the
    POSE_V2_HORSE_STEP_AUDIO lines of a secondary step).
A frozen puppet's ground plane goes stale (it lifts once and never lands
again) and its gait stays 0 (C +0x5DC is 0), so its own generator gives the
standing types. So a step is not recomputed from the puppet's physics:
each step event is recorded and replayed in normal play.

The mechanism: the recorder keeps every entry the engine put in a recorded

## Standing material of the feet (src/foot_material.h)

Engine facts (build 1491.50):
  - the foot audio (ped audio holder +0x1560, the foot vtable) keeps one
    64-byte record per foot at +0x240 (its constructor loops 4 times
    from +0x240 with stride 0x40). Its first 16 bytes are the main step
    container: +0x0 the container object (looked up from the ref), +0x8 the
    container name hash, +0xC the ref low dword (written by the slot
    writer's three stores, from the material settings of the foot,
    [foot +0x30 + 8 * foot], field +0x25C for a quadruped, +0x140..+0x148
    by shoe type for a biped).
  - the main step sound (called by the step sound) takes that slot
    ((foot index + 9) << 6, the slot read) to a ref helper, which builds
    the indirect ref container[index of the step name] (the child ref
    builder) and plays it (the step create and its create-and-play).
  - the foot audio update resolves the ground material of each foot
    every frame (a table lookup by the physics material id of the foot
    contact) and refreshes the slot (the slot writer) when that foot
    landed: a puppet's own probe decides the container of a replayed step.
  - the queued steps (entries of 16 bytes at +0x70, count at +0xB0, max 4,
    foot index at entry +0xC) are played by the step player inside the
    movement update (detoured, MovementUpdateDetour); nothing in the
    movement update refreshes the slots before the step player consumes
    the queue.

The mechanism: record, per foot-down, the slot the movement update played
it with (sampled at the movement update entry, paired here with the
foot-down); play it back by setting that slot on the puppet's foot before
the movement update consumes the replayed step.

## Ground layers (src/foot_material.h)

Engine facts (build 1491.50):
  - the main step sound (called for every step by the step sound) plays,
    after the main step, up to five layer sounds through the step layer
    (and its create-and-play): for layer i in 0..4, when
    [foot +0x3D0 + 4 * (5 * foot + i)] > 0 (the layer read), the container
    [layer table + 16 * (size class + 4 * i)] at that weight as volume;
    the size class is [ped audio +0x4608] +0x20, 3 when none;
  - the only writer is the layer update (its store: a per-layer
    ground query with attack/decay toward the previous
    value, layer 4 by a second query with the player bit of the owner),
    called by the foot audio's vtable slot +0x80 (the others: +0x10
    the reset, +0x18 the foot update);
  - the foot update calls [vtable +0x80] right before the movement
    update, which plays the queued steps (the step player): a value
    written at the movement update entry is the one the steps read, as
    the wetness.

## Wetness (src/foot_material.h)

Engine facts (build 1491.50):
  - the foot audio keeps one float per foot at +0x380 (the foot audio
    reset clears it); the foot update calls the wetness update after
    the movement update, and the wetness update sets
    each foot's value to the world ground wetness (the audio
    environment's +0x15F0, read by its getter) when that is higher,
    else lets it decay (1.0 while the ped audio's +0x774 says the foot
    is in water);
  - the main step sound (called for every step by the step sound)
    plays the wet layer of the step through the step layer (its create
    and play, the set 0x29934a for horses) only when
    foot +0x380 + 4 * foot > 0 (the wetness read) and the ped's +0xB0
    bit 7 is clear, with the wetness as its variable 0x02D01730;
  - so the step reads the value the previous frame's update left: the
    movement update entry is where the recorder samples it and where
    the replay sets it on the puppet's foot.
Without it the puppets' steps never play the wet layer.

## Per-foot mix of a hoof hit (src/foot_material.h)

Engine facts (build 1491.50):
  - the quadruped foot-down handler (called by the foot-down
    when [+0x4B8] type is 4) walks the gait's hit pattern
    ([foot +0x420] metadata, up to 8 entries of 0x14 bytes: type,
    volume, delay, high-pass, low-pass) and, for each hoof hit, calls
    the hit writer with the next foot ((foot + i) & 3);
  - the hit writer writes that foot's slot (+0x390 + 4 * foot volume f32,
    with the random +0x53C added; +0x3A0 delay; +0x3B0 high-pass u32;
    +0x3C0 low-pass u32) and then queues the step through the footstep
    entry, which the mod records (the delay goes as arg7);
  - the step sound reads the slot of the queued foot (its mix reads):
    volume += [+0x390 + 4 * foot], high-pass = max(params,
    [+0x3B0 + 4 * foot]), low-pass = min(params,
    [+0x3C0 + 4 * foot]);
  - the foot audio reset sets them to 0, 0, 0 and 24000.
A puppet's step is queued by the replay straight into that entry, so
its slots keep the reset values: every hoof hit at 0 dB, unfiltered
(a live gallop has about -2 to -6 dB and 50 to 300 Hz high-pass per hit).

