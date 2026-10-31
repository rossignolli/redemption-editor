# DrawableStore crash probe

Notes behind the DrawableStore probe (src/drawstore_probe_core.h), evidence
gathered for a crash at RDR2.exe+0x363068. RDR2 build 1491.50, offline dump.

## The crash

DrawableStore probe, evidence only, for a crash at RDR2.exe+0x363068.
Every observed instance destroys the same DrawableStore slot (local index
0xF7E5, the edi that 0x2B45BE4 passes to the store's Remove, saved at crash
rsp+0xF0 by 0x621A24). The crash is in that drawable's own destructor: its
shader group's page is decommitted while the shader group releases its
texture dictionary. The store took the "delete the whole object" branch
(entry flag bit 0), a bit the game's own load callback never sets in this
build. DRAWSTORE_SLOT logs that slot, the load callback and a census of the
entries with bit 0, so a crash names the asset and shows who set the bit.
Pure reads, no behaviour change.

## Layout

```text
Layout (build 1491.50):
  global      0x5AFA6B0, the store: static ctor 0xE8280 does
              lea rcx, [0x5AFA6B0]; call 0x2B85BF8, which passes the name
              "DrawableStore" (0x3670128) to the store base ctor and
              writes the vtable 0x366EBA0 (its only writer).
  vtable      slot 7 (+0x38) Remove 0x2BB1E10; slot 10 (+0x50) Load
              0x2BAB440; slot 39 (+0x138) GetSize 0x44130C (mov eax,[rcx+0x50]).
  store       +0x40 entries, +0x48 one flag byte per entry (0x80 = free),
              +0x50 entry count, +0x54 entry size (Remove 0x2BB1E10);
              +0xA0 load callback object, +0xA8 load callback function
              (Load 0x2BAB440 reads both; 0xAA9004 installs {0, 0xAB1510}).
  entry       +0x00 object, +0x10 name hash (RemoveSlot 0xEF0F78 passes it
              to the name map), +0x20 parent texture dictionary slot (Load
              0x2BAB440 pushes it as the current dictionary; -1 none),
              +0x24 u16 flags: bit 0 = Remove deletes the whole object
              (set only by Load when the callback says so; 0xAB1510 always
              says no), bit 1 = vtable slots 15/16.
  drawable    vtable 0x33C48D0 (gtaDrawable, dtor 0x61F564), +0x10 shader
              group (rmcDrawable dtor 0x26FC828 deletes it).
  shader grp  vtable 0x362EC90, +0x08 texture dictionary, released with
              lock xadd [+0x18] (dtor 0x2700618).
  dictionary  vtable 0x33BAD48 (pgDictionary<grcTexture>), +0x08 page map
              node (pgBase dtor 0x2601570), +0x10 parent dictionary and
              +0x18 refcount (dtor 0x559B88 releases the parent).
```
