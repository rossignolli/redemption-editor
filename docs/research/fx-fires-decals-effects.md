# Fires, decals and effects

Reverse engineering notes behind the replay of fires, decals, burn levels and
script or weapon effects. RDR2 build 1491.50, offline dump unless stated
otherwise. The code keeps the contract and a link here.

## Fires a clip starts with (src/fire_initial.h)

Evidence (build 1491.50):
- A fire is made by CFire::Init (and the field copy it calls first) from a
  descriptor; StartFireCore is one caller (every script, map, explosion,
  spread start), but the ptfx fire registration (reached from the particle
  instance update through a wrapper that passes type 11) calls GetFreeFire
  and CFire::Init directly: the start detour never sees those fires,
  whenever they start. The registration has other owners too (the entry
  update of a 16-entry system, type 11 as well; a vfx function, type 10;
  one more function, type 9), all with the owner in desc+0xB0:
  "ptfx-owned" below means registered by any owner.
- CFire::Init sets flags +0xE8 |= 0x180 when desc+0xB0 (the registering
  effect instance, kept at fire+0x1A8) is not null; the registration sets
  0x100 again each frame it finds its fire; CFire::Update shuts a fire
  with 0x80 and without 0x100 down. Such a fire lives exactly as long as
  its effect plays: it is the effect's, not a fire the replay can own on
  its own.
- The field copies of Init (desc -> fire): +0x00 -> +0x10, +0x10 -> +0x20,
  +0x20 (vec4, w = incident group, refcounted) -> +0x50, +0x30 -> +0x120,
  +0x40 -> +0x70, +0x48 -> +0x78, +0x50 -> +0x98, +0x54 -> +0x9C,
  +0x58 -> +0xA0, +0x60 -> +0x88, +0x68 -> +0x184, +0x6C -> +0x18C,
  +0x70 -> +0x190, +0x7C -> +0x14C, +0x90 -> +0x40, +0xA0 -> +0xA4,
  +0xA4 -> +0x140, +0xA8 -> +0x144, +0xB0 -> +0x1A8, +0xB8 -> +0x1A0,
  +0xC0 -> +0x80, +0xE0 -> +0x1C4, +0xE4 -> +0x154 (and the fire's seed),
  +0xE8 -> +0x90, +0x110 -> +0xDC; desc+0x114 bit 0x40 -> +0x184 = FLT_MAX
  (burns until stopped), bits 0x01/0x02/0x04/0x20 -> +0xE8 bits 11/10/12/13,
  desc+0x115 bit 0x02 -> +0xE8 bit 2. The defaults of the descriptor
  constructor fill the rest.
- StopFireInRange (the STOP_FIRE_IN_RANGE native) walks the
  active list (+0xB18B8 count, +0xB18BC int[128]), skips fires with +0xE8
  bit 11 (0x800) and shuts the others within the 3D radius of +0x30 down
  with Shutdown(fire, 0, 0).

## Weapon effect requests (src/weapon_fx_intent_core.h)

Why [binary]: the muzzle flash core (kWeaponMuzzleCore) tests the distance
to the camera position (against the info's range +8, squared) and the
frustum of the viewport (kWeaponViewport, +0x530..0x5A0: cmpleps/movmskps,
cmp eax,0xF) and leaves before it creates the effect through the triggered
instance wrapper; the shell core (kWeaponShellCore) does the same. Both are
called, twice each (a byte 0 then 1 as the last argument), by the request
wrappers (fx_observer.cpp): the muzzle one (rcx the manager kWeaponFxManager,
rdx shooter, r8 CWeapon, r9 weapon object, then the bone, the info, a vector
by pointer and a dword) and the shell one (rcx manager, rdx shooter, r8
CWeapon, r9 weapon object, two dwords: the bone is the second). Copying the
instances already made misses the flash of an NPC's shot outside the
gameplay camera's view.

The wrappers are detoured to record (they always forward). The replay calls
the same wrapper per recorded request (PlanCall below builds the arguments
from the puppets) and lets the in-game test decide about the sound: no
direct call path from either wrapper reaches the audio creates within 12
levels [binary], but 629 indirect calls stay unresolved, so every call is
measured (sounds of the shooter and its weapon right after it). The
viewport being the replay's free camera stays [hypothesis]; the playback
logs the engine's own plane test against the rendered camera.

## Script looped effects and the keep-alive get (src/script_fx_loop.h)

The engine:
  - the script fx object constructor (kScriptFxCtor) calls the script
    looped start, which gets or creates the registered entry through a
    wrapper with the owner key 0x10000000 (mov ecx, 0x10000000) and the
    script slot id ([slot+0x20]), the effect hash and the asset; the
    registered core is the one the fx observer detours, and the site it
    records for this start is the return into the constructor
    (kScriptFxStartCall, two frames above the wrapper);
  - every frame the script slot walk (called by kScriptFxKeepAlive, the
    recorded site is the return of kScriptFxKeepAliveCall) gets each entry
    of the active script slots again through the same wrapper with the
    same owner key and id but a hash and an asset of 0: the call only marks
    the entry as got this frame (+0x28), which keeps it playing (the
    registry update stops an entry not got). STOP_PARTICLE_FX_LOOPED clears
    the slot and the entry is not got any more.
Dropping the calls without an effect hash would record only the start of a
script looped effect (e.g. the dynamite fuse, proj_dynamite_trail). A call
without a hash counts for the live key it gets (same owner and id), with
that key's effect; a key not live yet (a script effect started before the
recording) takes the effect the instance holds (rule +0x200, the same read
GuardedClearBit compares with the track's hash).

## Particle relocation, registry restart and attach frees (src/fx_motion_rule.h)

Engine facts:
- Only two writers set bit 0x2 of an instance's +0x584 ("relocated"): the
  relocation mark (fx_observer.cpp kFxRelocateMark: every instance attached
  to an entity whose transform a script native set,
  SET_ENTITY_COORDS_NO_OFFSET through the entity's vfunc +0x250) and an
  attached instance's parent test, when the parent moved more than the
  instance's +0x3A8 in one update, states 2..4. The replay places every
  puppet with that native every tick, so every effect attached to a puppet
  is likely marked relocated every frame (unconfirmed; FX_RELOCATE_PROBE
  logs it), a jump for the emitter.
- The registry update (kFxRegistryUpdate) stops softly an entry not got
  this frame and restarts (the instance start, kFxRegistryRestartCall) an
  entry got again in state 3..5: the hoof dust is a burst per call run, not
  a stream.
- The attach update frees an entry whose entity reference is null, whose
  bone matrix has a zero X axis outside its bone table, or whose entity's
  draw data [[e+0x28]+8] is null (vfunc +0xA8); the next get of the key
  creates the instance again.

## Burn level block (src/burn_level.h)

```text
Every shader effect instance (CCustomShaderEffect, the object at draw
handler +0x18, draw handler = entity +0x28) owns a block of instance
shader variables that its base constructors allocate (the base one, the
ped's and the vehicle's) and keep at effect +0x18; the virtual at +0x88
returns it (a jump into the protected text, `mov rax, [rcx+0x18]`, the slot
of the 32 effect classes that have one; types 4 and two others return
null). Its common header holds the burn state:
  +0x1C burn level (its setter): the fire's burnable entity update, the
        vehicle's burn update (never lowered unless forced) and
        _SET_OBJECT_BURN_LEVEL(o, l, affectAsh=1) write it; its getter
        reads it
  +0x20 the same native with affectAsh=0
  +0x24 burn intensity, +0x28 burn opacity (their natives; zeroed and
        refilled every frame while the fire's entry is alive)
  +0x30 entity mask colour (RGBA8)
Peds keep a second effect: the engine's own resolver (entity mask
natives) takes [[draw handler +0x748] +0xC0] for a ped (entity type
byte +0x30 == 4) instead of +0x18. Both blocks are read and written.
No native reads the level and none writes it on a ped or vehicle, so the
replay reads and writes the block itself. Found by signature once (Check),
SEH-guarded; never calls engine code.
```
