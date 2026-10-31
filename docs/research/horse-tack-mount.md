# Mount, rider and horse tack on replay puppets

Reverse engineering notes behind the rider placement on a mount puppet and the reins guard (RDR2 build 1491.50, offline dump unless stated). The code keeps the contract and a link here.

## Rider follows the mount at pre-render (src/mount_placement.h)

The galloping horse puppet is a physics body (for the plants); the engine
moves it 4 to 11 cm off our placement while the frozen rider stays where
the recording put him. As a replay editor usually does, the child follows
its parent in the same frame and the entities are written at the very end
of the frame so nothing overrides them.

Mechanism: a detour on the ped pre-render, the last per-entity step
before the frame is drawn. At a rider puppet's pre-render, before the
original runs, the rider's transform is set to the mount puppet's
transform at that moment composed with the recorded rider-in-mount
offset of this tick (child follows parent). A horse that was not moved
gives the recorded rider placement back (no write); a pushed horse
carries its rider along. Only replay pairs the playback published this
tick are touched; every other ped goes straight to the original.

Engine layout (the signatures are in mount_placement.cpp):

```text
- The ped vtable (the one the ped constructor stores with the entity type
  4): the only .rdata vtable whose +0xB8 slot is the ped fragment getter
  (skeleton_walk.h kPedGetter); it shares the entity and physical slots
  with the vehicle vtables. Horses use it too (their skeleton walk
  classifies as ped).
- Slot +0x258 = the ped pre-render (.pdata start, no direct caller, only
  that vtable slot points at it): the same slot as the vehicle pre-render
  (pose_capture.cpp).
  It is called by the pre-render list loops (one call per listed entity,
  the argument says whether the entity was visible in the main viewport
  this frame), which run inside the pre-render job over six entity
  lists. The body reads the entity's final position (+0x70, cells) for
  its lighting probe. No position encoder is reached from it in five
  levels of direct calls (virtual calls not followed): it does not move
  the ped.
- Entity matrix +0x40 right, +0x50 forward, +0x60 up, +0x70 position
  remainder with the 32 m cell indices (int16 x, y) in the w lane:
  GET_ENTITY_MATRIX's handler reads it so, and the position encoder
  writes it (cell = truncation of position / 32 toward zero, z kept
  whole).
- A ped's skeleton is the fragment one (fragment_path=1); its parent
  matrix is [cacheEntry+0xE8]+0x40 (the fragment cache entry builds its
  skeleton with it; the skeleton init keeps it at +0x08), the physics
  instance matrix, written by the position encoder with the same layout.
  So a ped has two copies of its transform; both are written here.
- The order "physics, then script, then pre-render, then draw" (pre-render
  the last step of the update) is a hypothesis for RDR2. The evidence
  fields below show whether the horse is already off its placement at the
  pre-render (the jolt is drawn and this follow removes it) or only later
  (the jolt is never drawn and the next-tick detector counts something the
  screen did not show).
```

Rungs: 5 (detour by signature, the vtable slot and the ped getter checked
at load, forwards every call, acts only on replay riders) and 3 (guarded
writes of the rider puppet's two matrices, whose layout the engine code
above proves). The detour never calls a native; the script thread
publishes the pairs and reads the evidence back.

## Reins on the saddle (src/reins_guard.h)

A horse's reins are an engine rope (type 5, 39 vertices) with 24
attachment entries (rope +0x2C0, stride 0x80): 0 and 1 the bit, 2 the
saddle (bone 62111, vertex 19), the others the hands of whoever holds it.
The horse's reins controller (ctrl; rope at [[ctrl+0x10]+0x7A0]) keeps a
state bit, ctrl+0x70 bit 2, "the reins are off the saddle, in a hand".
Build 1491.50:

```text
  - the take (ctrl, &offset): takes the reins off the saddle (entry 2 set
    to null through the attachment setter, entry+0x70 and
    +0x74 = 1) and sets bit 2. Called by an event handler and by one
    more function.
  - the put-back (ctrl): when bit 2 is set and bit 14 is not, puts them back
    (entry 2 = the entity of entry 0, the horse; entry+0x74 = 0) and
    clears bits 2 and 13. Called by the controller update
    (the in-hand vertex too far from where the controller expects it),
    by the same event handler as the take and by one more function.
```

A puppet horse is not led by anyone the engine knows, so in playback its
controller calls the put-back every frame while the recorded entries say the
saddle entry is empty and the rope's middle is in the puppet's hand
(without the guard the reins hang in stretched loops from the bit through
the saddle and the hand). The replay owns every rope pin of a replay
entity while it plays; the detour skips the put-back for ropes the
playback follows and forwards every other call. The recorded entries
(including 2 = the horse when the recording had the reins on the saddle)
go through the setter.

## Wagon reins (src/reins_guard.h)

The wagon reins. A draft
vehicle's reins are the draft horses' reins ropes (type 5, 43 vertices, 12
entries: 0..3 the horse, 4 the wagon's rest pin, bone 7434, 5..9 the hands
of the driver, 10 and 11 the other horse). Each rope's controller has hold
modes (its tuning at [ctrl+0x28]+0x70, stride 0x20: the slots of the mode
and which of them take the holder). Build 1491.50:

```text
  - the hold mode setter (ctrl, holder, mode): sets the slots of `mode` to
    `holder` or null through the slot setter (attachment setter,
    entry +0x70 active, +0x74 cleared) and keeps the mode at ctrl+0x60;
  - the draft vehicle's reins update (one caller),
    every frame for each of its six controllers (draft object at
    vehicle+0x23D0, controllers at +0x18): with the driver seat empty it
    calls the hold mode setter (ctrl, vehicle, 0) (two sites), mode 0 = the
    reins rest on the wagon: entry 4 = the wagon, the hand entries null;
    with a driver, the hold mode setter (ctrl, driver, mode);
  - the driver comes from the vehicle's seat manager (a getter reads the
    occupant of the driver seat, [seats+0xB0+8*[seats+0x21]]), the same
    answer the natives PHYSICS 0x31160EC47E7C9549 (mode 0..3) and
    0xEAF529446488EB18 (mode 0) get.
```

A puppet driver rides its recorded pose and is not in the seat (the seat is
data, replay_playback.cpp), so on the puppet wagon the update would put the
reins back on the wagon every frame over the recorded hand entries (slot 4
the wagon, slots 5..9 null). Like the saddle put-back, the hold mode change
is skipped for ropes the playback follows (the replay owns every pin of a
replay rope) and forwarded for every other rope. Ini ReinsHoldGuard (1).

