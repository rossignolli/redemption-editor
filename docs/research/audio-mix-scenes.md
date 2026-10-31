# Audio mixer scenes, mix groups and sound categories

Engine notes behind the recording and replay of the dynamic mixer's scenes,
the entities' audio mix groups, the mixer's scene pool and the sound
categories' controller values (RDR2 build 1491.50, offline dump; the
signatures that hold the bytes are in the source files named in each
section).

## Mixer scenes and mix groups (src/mix_scene_core.h)

Engine facts (the signatures are in mix_scene.cpp):
  - the scripted conversation manager's update picks the dominant playing
    conversation (state 3 at +0x130, the highest priority +0x144, then the
    nearest) among its conversations (+0x860, count +0x980), stops the
    scene of every other one (the conversation stop: conversation +0xE8 ->
    the mixer stop) and starts, once, the scene 0x5B9DE6B4 on the dominant
    one (the conversation start -> the mixer start, scene pointer kept at
    conversation +0xE8);
  - that scene gets a filter (the conversation start): a list of the
    conversation's participants' tags (participant array +0x30, count u16
    +0x38, stride 0xA0: tag u32 at +0x7C for each participant with the byte
    +0x98 set), mode 1 and invert 1, plus the metadata 0xB77A4E4D; the
    mixer's match compares the list with the audio mix group hashes of the
    sound's entity (entity audio vtable +0x78, words +0xEC, hashed).
    Inverted: the scene acts on the sounds of the entities that are NOT in
    the participants' mix groups. That is the conversation duck: everyone
    but the speakers;
  - the replay says the lines one by one on the direct path, so no
    conversation exists and the manager starts no scene: without this,
    ambient chatter plays at its normal level under a conversation;
  - an entity's mix group is set by ADD_ENTITY_TO_AUDIO_MIX_GROUP (its
    handler -> the group add (mixer, entity, slot 0, hash, fade)) and read
    by _GET_ENTITY_AUDIO_MIX_GROUP (slot 0, 0 when none);
  - a script scene lives in the calling script thread's slot of the script
    audio object (+0x37D0, 32 slots of 0x380 keyed by the active thread's
    id); the script start returns true without starting anything when that
    slot already holds a scene of the hash; the hash variant of
    START_AUDIO_SCENE is the native 0xDC2F83A0612CA34D and of
    STOP_AUDIO_SCENE 0x6AB944DF68B512D3: the playback uses them;
  - a scene's variables (SET_AUDIO_SCENE_VARIABLE, and the engine's own
    variable set (scene, name hash, value)) are the array of 10 {name hash,
    value} at scene +0x18, count at +0x68; the recording polls it and the
    playback sets the recorded values with the variable set;
  - the engine's code scenes (caller other than the two above) are started
    by systems driven by the game state (weather and ambient zones, the
    refcounted scene table, the audio engine's update, ...); the playback
    may start them again directly in the mixer (the mixer start with a
    reference of the replay's own), only when the engine does not already
    have a scene of the hash open (then the engine owns it), behind
    MixSceneReplayCode (default 0);
  - a stop whose scene no longer carries the instance's hash (+0x328) is a
    stale reference; the recording polls each open scene (+0x310
    reference, +0x328 hash) and ends it when it vanished.
So the recording keeps every scene start and stop (hash, who started it,
the participants' tags of a conversation scene), the scene variables and
each recorded ped's mix group; the playback puts the puppets in the
recorded groups and starts the conversation scenes again with the recorded
tags (a conversation object of the replay's own) and the script scenes by
hash.

## Mixer scene pool and driver scenes (src/mix_scene_pool_core.h)

The engine (the signatures below hold the bytes these facts come from):
  - the pool start (mixer, settings, &reference, ...) takes a slot of 0x340
    bytes from the scene pool without checking the hash, constructs it (the
    constructor zeroes +0x328), sets the settings (+0x308) and, with a
    reference, writes the scene there and the reference's address at +0x310
    (kSceneReferenceStore); only the wrapper writes the hash at +0x328
    (kSceneHashStore). A failed or full pool returns 0 without touching the
    reference, and every caller releases its temporary list afterwards (the
    release handles an empty one);
  - the pool's pointer is kept encoded in three cells: a byte (set: valid),
    the value and the key, decoded as the pool start does (ror 5, rol 32,
    xor, rol 32, rol by (key & 31) + 1, not); the pool: +8 base, +0x10 the
    flag bytes (a slot is in use when its flag has bit 7 clear), +0x18
    count, +0x1C stride (the pool update's loop over the slots);
  - a scene is ready to be deleted when its 16 patch slots (+0x70, stride
    0x10), its 16 slots at +0x170 and its list at +0x270 (count +0x2F0)
    are all empty (kReadyToDelete): a stopped scene clears them and its
    reference (+0x310, and the reference itself);
  - the drivers: the duck driver (the speech duck 0x576520E1 and the second
    duck 0x0E585610) and the game state driver (ten scenes by game state)
    start their scenes straight into the pool start, so the hash is never
    at +0x328; each keeps its scene in a reference of its own, used by that
    one start site (the ref names the scene). The player state driver
    starts 0x24BFC688 and 0x2348B998 through the wrapper by the hidden live
    player's state (bit 14 of +0x3F50 of the local player's component), and
    when the start gives no scene it tries again the next frame (its test
    of the reference after the start).
Observed live: the speech duck 0x576520E1 is live exactly in the recorded
conversations; 0x24BFC688 would follow the hidden player in the replay; and
a list of open scenes kept from the starts alone misses the session's
permanent scenes when the hooks warm up late, so the replay would start
second instances of them.

## Sound categories (src/category_state_core.h)

Engine layout (the signatures below hold the bytes it comes from):
  - the resolved volume of category i (the resolved table, stride 0x48, +0
    in dB, what the volume probe calls cat_db) is written by the resolver
    (category, u16, use meta) on the mixer side: meta (+0, only with the
    third argument set) plus the controllers' sum (+0x48) plus the parent's
    resolved value (+0xB0 the parent's index, unless +0xE4 bit 0), stored
    at the category's own index (+0xB2). It is called for the root (use
    meta 1), for each dynamic hierarchy and recursively for the children,
    so a parent's value is final before its children read it;
  - the controllers' sum of every static category (the static count of
    them, the category pool: +8 base, +0x10 count, +0x20 stride, +0x38 the
    allocated bits) is written by the controller manager (game side, its
    own lock +0x840): +0x48 volume dB, +0x4C and +0x54 (multipliers), +0x88
    pitch, +0x8A low pass, +0x8C high pass, and 0, 1, 1, 0, 20000, 0 for a
    category without a controller. A controller comes from a mixer scene's
    patch or from code (both through the controller's creation): the
    sliders SFX_SLIDER and MUSIC_SLIDER and the music categories
    INTERACTIVE_MUSIC and MUSIC_ONESHOT;
  - a category by hash: the category map (+0xA4 count, +0xA8 entries of 16
    bytes {category pointer, hash}, sorted by hash, its lookup); the pool
    index of a category is (pointer - base) / stride (the controller's
    creation).
Without this, the category stage (d_cat) differs from the recording by
about +4 dB on scripted speech and +2 to +6 dB on ambient speech, ambience
and weather, from mixer scenes the replay session has and the recording did
not (and the other way round): the conversation duck, the hidden live
player's state scenes, duplicated permanent scenes.
