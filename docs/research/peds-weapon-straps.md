# Weapon straps and weapon components on puppets

Reverse engineering notes behind the strap state and the weapon component order the replay puts on a puppet weapon (RDR2 build 1491.50, offline dump plus in-game tests where stated). The code keeps the contract and a link here.

## Strap state flags (src/strap_state_core.h)

The strap state of a weapon as the replay plays it back, pure and unit
tested (QA report 232-Valentine: the red cloth sling of a bolt-action rifle
stood up in rigid loops while Arthur aimed).

The recorder keeps, per weapon object with straps, every change of the
strap state (weapon_observer.h StrapState: the component hide bits, the
whole weapon flags word [CWeapon+0x62C] and the two blend weights; the STRP
section, unchanged). Playback puts that state on the puppet weapon.

The flag bits of the weapon flags word, offline (weapon_observer.cpp has the signatures of the weapon pass and of the manager's strap writes):

```text
  0x00000004  set by the per-weapon pass once the weapon's matrices are
              built this frame, cleared at its start. Not a strap bit.
  0x00000080  one-shot request read and cleared by one weapon function. No
              strap code reads it. Not replayed.
  0x00000800  "strap held by the weapon task" (A): set when the weapon
              task's state 3 starts, cleared by its update and exit and by
              the holster paths (with the in-hand bit below).
  0x00001000  "strap held by the weapon task" (B): set every update of the
              same task's state 5 (its strap hold setter with 1, on the
              weapon of both item slots at task+0x4A0) and cleared on its
              exit (the same setter with 0).
              The owner ped's weapon manager tests 0x1800 (three times):
              with either bit set it keeps its strap choice, does not write
              the hide bits, the 0x20000 state or the weights, and does not
              request the rebuild (0x8000). The recording shows it: the rifle
              51:1574 aims from 8.209 s to 23.045 s with 0x233084 (0x1000
              set, 0x8000 clear) and is 0x238084 on the back (0x8000 set).
  0x00002000  the weapon object is held in a hand: set by the equip paths
              (four of them, through a setter which also stamps +0x624),
              cleared by the holster natives. Readers: the held test (held
              and not one of four excluded weapons), which makes the object
              pass run the whole weapon process for the held weapon every
              frame, and an object predicate. Not a strap bit; not written
              on a puppet (that would run the weapon process on it). Read
              only, to choose the rebuild below.
  0x00008000  "rebuild the straps from the attach parent ped": set every
              frame by the weapon manager when it drives the strap (never
              under 0x1800); consumed by the object's skeleton callback
              (registered for a weapon with straps; runs only with 0x8000
              and 0x10000), which re-poses the strap bones from the ped's
              (bone 0x5842 of some rifles, then the 10-bone cloth chain with
              weight +0x5D8 and the shoulder strap with weight +0x5D4, each
              only while that strap is shown) and writes their object
              matrices, after the pose the replay wrote.
  0x00010000  gate of that callback (set at weapon init).
  0x00020000  set by the weapon manager when its per-hash strap state is
              2 (the table at manager+0x408); the callback picks the
              weapon's anim parameter 0x3E89 from it. Replayed as before.
  0x00200000  read only by script natives; no strap code. Not replayed.
```

### The sling defect and the rebuild rule

The defect: playback set 0x8000 on every frame, so the callback re-posed
the visible cloth chain from the ped with the recorded weight (0 while
aiming) over the recorded pose, every frame, where the engine had left the
strap to the weapon's own pose. Playback now asks for the rebuild only
where the engine did: in a state recorded with 0x8000 (the shoulder sling
on the back, holstered weapons: as before), or in a state recorded with no
sign of being in a hand (neither the hold bits nor 0x2000: as before, so
nothing that plays well today changes). A state recorded in a hand without
0x8000 (the cloth sling while aiming) gets no rebuild. The hold bits and
0x20000 are replayed as recorded.

### In-game check

In game (a QA clip at 13442 ms) the flags are played exactly
(read back 0x31000) and the loops stay: they are not a strap. The bench op
strap_test (developer build) hid one component at a time on the puppet:
hiding COMPONENT_RIFLE_BOLTACTION_WRAP (0x549A7FC8) removes the leather
loop; hiding REPEATER_CLOTH_STRAP01, the scope, the sight or the grip, or
showing REPEATER_STRAP01 or the shoulder sling, changes nothing; clearing
0x1000, 0x20000 or 0x200000 or forcing either weight changes nothing. So
the fallback once planned here (hide the cloth strap, show REPEATER_STRAP01)
is wrong and was not made. The cause is the order the puppet got its
components in: the wrap before the scope (weapon_order_core.h).

## Component order: the wrap before the scope (src/weapon_order_core.h)

The recorded component list is in hash order. A bolt-action rifle with a
scope and COMPONENT_RIFLE_BOLTACTION_WRAP (0x549A7FC8) gets the wrap
before COMPONENT_RIFLE_SCOPE03 (0x920B56D7) in that order. In game the
puppet then draws no scope at all (HAS_WEAPON_GOT_WEAPON_COMPONENT says it
has it, the mask bit shows it) and the wrap in the shape it has without a
scope: two stiff leather loops standing above the receiver and stock, the
"red loops" of the report. Removing the wrap and giving it again once the
scope is on (bench strap_test regive 549a7fc8) draws both as the live game
does: the scope on the receiver and the wrap flush. Giving the scope again
alone changes nothing; nor does any strap flag, weight or mask bit.

