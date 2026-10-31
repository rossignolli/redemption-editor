# Map data objects by their owner

Notes behind the owner rule in `src/object_check.h` (`kOwnerMapInstance` and
its siblings): how RDR2 records, in the entity's owner field, whether an
object came from the map data (offline dump, build 1491.50).

A replay editor treats an object of the map data (an IPL instance) as
map-object state and gives every other object a create. RDR2 keeps that
origin in the owner field itself:

  - 25: a real object made from a map data instance. A builder walks
    the queued (map data slot, instance index) pairs of the map data
    store, builds the object, sets the owner bits to 25 (two stores)
    and calls a helper, which stores the slot at +0xD8, the
    instance index at +0xDA and 1 at +0xDC and puts the object in the
    store entry's per-instance table (+0x280). The object removal
    releases that table slot only for owner 25 (cmp
    0x64000000). No other site writes 25 as a constant
    (no SetOwnedBy caller loads it, no other store of
    0x64000000 at +0xB4; the lend's give-back restores the
    owner it kept).
  - 14: an entity set up from a map data entity definition (the setup:
    the definition's flags, archetype and tint, then the owner bits
    to 14).
  - 17: the entities an interior instance adds (the adder passes 0x11
    to SetOwnedBy for each); another function walks a map data entity list
    and acts on owner 14 or 17 only.
  - 24: a map object the engine lends for a while (the lend keeps the
    old owner at +0x798 and sets 24; the give-back restores it).
So the map's owners are exactly 14, 17, 24 and 25 (they also held
97.8% of the objects that were no scenery in 67 snapshots); a "most
frequent owner" learned at the snapshot is not part of the rule (it only
adds noise: a camp's map owner was 25, a town's 14 or 17). CREATE_OBJECT
gives 5, a prop set 23, the temporary objects 2 or 3; every other value
(0, 1, 4 the persistence system, 7, 11, 18...) is no map data either.

## Map dressing and its cap

A mission's camp (tent and boxes) can be map data of the mission's IPL
(every snapshot object there owner 25, the map data instance's, none
scenery); a playback in a mission only counts the world state's
difference (RestoreWorldStateInMission=0), and the closed theater's
clear removes a town's map data too (a store's shelf). So the map data
near the player (nearest first) is kept as a map dressing entry with the
clip IPL whose sphere holds it (0: the always-on map), and the playback
builds it only when the map does not have it: its IPL off, or its
original still missing after kMapDressingWaitMs.
Nearest first within the snapshot radius: each entry is watched with
the scenery (four times a second). The closed theater clears the town's
map data, so every map data object of the snapshot is kept: the
snapshot lists at most 1024 objects (recorder::kPoolCapacity), so the
cap is that list. The old cap of 192 left Saint Denis 274 to 458 short
and Blackwater 69 (built_missing at playback). Measured on the QA logs
(rec.scenery, PROFILE_ZONE): about 1.5 us per watched entry
per scan (320 us for about 200 entries), so 650 map data objects cost
about 1 ms every 250 ms; play.object_stands stays under 35 us a frame.
