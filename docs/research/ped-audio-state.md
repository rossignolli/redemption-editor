# Ped audio state

Reverse engineering notes behind the ped audio of the replay's puppets: the
foley variants, the ped variable blocks, the speeds the step sounds read, the
horse audio evidence and the animal sound banks. Build 1491.50, offline dump
unless a note says otherwise. The code keeps the contract and links here.

## Foley variants and ped variables (src/ped_audio_state.h)

(a) Foley variants. The ped audio holder (two kinds, each made by its own
constructor on top of the holder's: three vtables) has at +0x210 a
component (its constructor writes -1 to +0xEC and +0xF0). Its activation
(vtable slot +0x68) draws the two variant indices once, when +0xEC is still
-1 (the draw): the local player gets 0 and 0 (component row +0xB8 flags bit
1 set and bit 0 clear, the same test as the weapon audio's predicate);
every other ped, horse and animal gets 1 + floor((N - 1) * rand / 32768)
from the game RNG, N two audio tunables (+0x60 and +0x70). The activation
also requests a resource into +0x168 when either index is 2 or 3 (the
deactivation does not release it). The reader copies the two indices into
the init params of every sound its 26 callers create (variables 0x99789761
and 0xDF33D3DE, through the params' variable setter): the sound's variant
is chosen by them. A puppet is never the local player, so left alone the
recorded player's puppet would draw random variants where the player had 0
and 0, and every other puppet would draw new ones.

So each recorded owner's two indices are recorded once (clip section FVAR)
and written into its puppet's component, the player's 0 and 0 included. A
clip without the section gives the recorded player's puppet the live
player's indices.
A puppet whose activation already ran without the resource of an index 2
or 3 is left as it is (the resource is requested only there).

(b) Ped variables (evidence only). Each ped audio holder owns a variable
block named PED_VARIABLES (0x549DD7FB, made by the block alloc; its index is
the word at holder +0x12, -1 none; the blocks are 16-byte entries
{descriptor, values} in the block table; the descriptor holds the count at
+8 and the names at +0xC + 8 * i; the values are floats, the block write
writes them). The ped variable update writes them each audio update through
the block setter; among them is_player (0x836BD219) and 0xA1691FFD (both the
row +0xB8 player test), 0xDB583619 (holder +0x4662 bit 0, the player or
focus flag) and, for the player only, 0x4A276B23 (the bits of
joaat("player")). A sound reads an entity variable through the variable
lookup: its own variables first, then the global ones, then the block whose
index its request copied from its entity at init, then the global
categories. The replay's evidence: the block of each recorded owner
(record) and puppet (play), and which names the sounds resolved in those
blocks (a read-only detour on the lookup, ped_audio_state.cpp).

## The movement component's speed (src/ped_audio_state.h, MotionSpeedReplay)

The block speed of (c) is a copy: the engine reads the movement
component's own speed C +0x5DC (C = row +0x88) in the step volume curve,
three other ped audio reads and the gait classifier (the gait C +0x630 from
+0x5DC against the thresholds of [C +0x4C0]), and the gait picks the
per-gait lift thresholds, the foot routine and the type of the secondary
steps. The only writer of +0x5DC is the movement speed update: the
smoother over the raw speed (C) = |physical velocity (row +0xE0 vtable
+0xC8) - ground velocity (C +0x5A0)|, 0 on a frozen puppet. A detour on
the raw speed (its prologue, and the writer, the sound and the gait read
checked) forwards every call and, in normal play, answers the published
puppets' recorded speed (the target of (c): PSPD, else the recorded
motion). The smoother's rates do not smooth at ped speeds (ped_speed_core.h).

## Player horse step sounds (src/horse_audio_evidence.h)

Offline (build 1491.50, .text dump, capstone):
  - the queued hoofsteps (foot audio holder +0x1560, queue +0x70, count
    +0xB0, filled by the footstep entry 0x317A340) are played by the
    movement sounds 0x3193F20, one call of 0x318D52C per step; the main
    step sound is the create-and-play at 0x318DAF9 (return 0x318DAFE) with
    the ref from 0x317FC9C (foot +0x428 settings, when component row slot
    +0x00 is set) or 0x3182A98 (the ped audio's +0xE00 object, settings at
    its +0x650 = ped audio +0x1450, when slot +0x00 is empty). Both pick
    the hash at settings +4 * step type and resolve it (0x252B408).
  - the +0xE00 object's +0x650 is written by 0x319E8D8 for slot 10 only
    (0x3182A2C: game object type 0x16 from the slot 10 hash at +0x498,
    fallback 0x3D857DBD); the eleven slot hashes +0x470..+0x498 come from
    0x317C8D4 (the ped's audio component, vtable +0x80, 0x3276F78: tag per
    slot of the ped's outfit entries; empty slot 10 without row +0x00 ->
    0xDE27F155 or 0xDE27F155 + 0x863F3CF1 by 0xF0AB1C). The update that
    refreshes them (0x3191324, vtable 0x3839338 slot +0x18) first runs the
    player horse branch choice 0x319EEE8.
  - 0x319EEE8 (same object): +0x700 set, row +0xE0 component +0x400 bit 6
    clear and ped audio +0x45E8 <= 1, then with row +0xA8 component C:
    0x8B7498(C) || 0x8B69B0(C) || 0x8B4A88(C) || ped audio +0x4662 bit 0
    -> 0x319CF14 (player horse), else 0x3198574; no C -> 0x31A1900.
    0x8B69B0: [C+0xB0] entity, its row +0xB8 component +0x40: bit 1 set,
    bit 0 clear. 0x8B4A88: [C+0xA8] entity has row +0xF0. 0x8B7498:
    E1 = [C+8] & ~0xF, C0 = row(E1) +0x00, E2 = [C0+8] & ~0xF, then an
    obfuscated pointer of E2 (0x2B1990) and a task list (0x737B78) whose
    entity has row +0xB8 +0x40 bit 1 (any player). Only E1 and C0 are read
    here; the rest is decoded by the engine only.
  - movement sounds 0x3193F20 set init params +0x138 = 8 when 0x8B69B0
    says yes (0x31941F9), the params every queued step plays with.


## The horse's persistent loops (src/horse_audio_evidence.h)

  - +0xE00 object +0x30 and +0x48: the saddle weapon loops of
    0x31A0F30 (called by the +0xE00 update 0x3191324 at 0x31914A6;
    it walks the ped attachments 0xCC8C58 and keeps one slot per
    carried weapon, created through 0x319D96C and the wrapper
    0x2588BE0, stopped with 0x2585C2C when the weapon is gone);
  - foot +0x596: the tack loops' switch, set by 0x3197C78 at
    0x3198099 (from the game code 0x8EECE8, state 10), cleared by
    the foot update 0x31925AC at 0x3192824 and by 0x31986BC;
  - foot +0x4D8 and +0x4E0: the tack loops of 0x319F8A4 (foot
    vtable 0x38389B0 slot +0x80 through 0x319CBF0), entry
    0x63D72B3C of the sets at foot +0x240 and +0x280, created
    through the wrapper (returns 0x319FC53 and 0x319FEB0) while
    +0x596 is set, then moved and driven every update (0x2537168,
    0x2536C70, 0x2536E48, variables 0x833FF9C9, 0x48FE569D and
    0x41F750D5 = foot +0x580 is 0x41F750D5);
  - foot +0x480 and +0x5A5: the urge loop of 0x31A1420 (called by
    the foot update at 0x319290B when 0x8B69B0 says yes), created
    through the wrapper (return 0x31A1593) while +0x5A5 is set.

