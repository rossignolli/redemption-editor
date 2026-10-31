# Entity pools and the per-frame entity cache

Notes behind `src/entity_pools.h` and `src/entity_cache.h`: how the recorder
reads the engine's ped, vehicle, object and script handle pools directly
(RDR2 build 1491.50, static analysis of the offline dump). Every layout fact
below is signature-checked at runtime by `pools::Check`.

## Why not the ScriptHook listing

ScriptHookRDR2's worldGetAllPeds/Vehicles/Objects give every listed entity a
script handle (an fwScriptGuid) and stop listing while that pool has 256 free
slots or fewer (`cmp free, 0x100` in the export). In a town they return 0 most
of the time and the recorder goes blind.

## Pool layout

```text
- rage::fwBasePool (its ctor, GetAt and Delete):
  +0x08 storage, +0x10 flag bytes (one per slot: bit 0x80 free, the low
  7 bits the slot's generation), +0x18 slot count (int), +0x1C entry size
  (int), +0x20/+0x24 free list ends, +0x28 used count (low 30 bits). The
  entity is the entry itself: storage + index * entry size. A pool
  reference is (index << 8) | flag byte.
- The pool pointers are stored encrypted: a flag byte at G-8 (0: no
  pool), the value at G, a 64-bit key at G+8. Every reader decodes
  pool = ~rol(rol(value ^ k, (k & 31) + b), 32), k = rol(ror(key, a), 32).
  Peds ("Peds", entry 0x380, CPed allocations of 0x380):
  G from the switch, a = 2, b = 3. Vehicles ("CVehicle", entry = the largest
  vehicle class, allocations 0x10E0 to 0x1B80): G from the switch, a = 1,
  b = 4. Objects ("Object", entry 0xCD0, allocations 0x7A0 to 0xCD0):
  G from the switch, a = 2, b = 2. Script handles ("fwScriptGuid", entry
  0x10: +0x08 the entity): G from the guid maker, a = 2, b = 3. The
  engine's own pool switch (type 1 vehicle, 2 ped, 3 object) holds the three
  entity decoders; that function is the signature.
- Entity +0x30: type byte (3 vehicle, 4 ped, 5 object: the IS_ENTITY_A_*
  cores). Entity +0x70: position, x y z floats plus two int16 at
  +0x7C/+0x7E that add 32 m steps to x and y (GET_ENTITY_COORDS through
  its reader, the step constant {32, 32, 0, 0}).
- Entity to script handle: the jump to the guid maker, the path the
  natives take: returns the entity's handle, allocating one only when it
  has none. Handle = (script guid slot << 8) | its flag byte.
```

## Entity cache

Asking the ScriptHook the same questions about the same entities every frame
from several modules (DOES_ENTITY_EXIST, getScriptHandleBaseAddress,
IS_ENTITY_A_PED, GET_ENTITY_COORDS, _HAS_PED_TAKEN_GORE_DAMAGE x9) costs about
1,700 to 2,000 calls per recording frame with 56 peds.

The script handle pool (fwScriptGuid) is decoded once per script tick; a
handle then resolves the way the engine's GetAt does, by reading memory only:
the slot's flag byte must equal the handle's low byte, the guid's +0x08 is the
entity, the entity's +0x30 its type and +0x70 its position. Each answer costs
three or four guarded reads and no ScriptHook call. The cross-check compares
with DOES_ENTITY_EXIST, getScriptHandleBaseAddress and GET_ENTITY_COORDS.

### Gore

The engine's per-ped gore entry (a global table pointer and u16 count, entries
of 0x20: +0x00 ped, +0x18 mask) is read once per frame;
_HAS_PED_TAKEN_GORE_DAMAGE's core is `bt [entry+0x18], limb`, that is bit
(limb & 31) of the 32-bit mask (limb 37 is bit 5). Getter and core are
signature-checked.
