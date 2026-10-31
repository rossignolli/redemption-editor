# Speech duck, slots and conversations

Reverse engineering notes behind the replay of the speech manager's shared
state: the speech duck the conversation manager raises, the ambient speech
slots, the conversations, the resume of interrupted lines and the animal voice
banks. Build 1491.50, offline dump unless a note says it was measured. The code
keeps the contract and links here.

## The speech duck driver (src/speech_duck_core.h)

Engine facts (the signatures below hold the bytes these come from):
  - the driver (no arguments; called once per audio update by the game
    state driver, mix_scene_pool_core.h) reads the duck level (kLevel):
    above 0 and with the block gate not above 0 it starts the scene
    0x576520E1 into its reference (straight into the pool start) and sets
    seven variables on it: apply 0xE865CDE8 = level; 0xCC9C15B6 = level *
    (EDE ? curve 0x4D73F743 : 0); 0x015D3903 = level * EDC; 0xE269C300 =
    level * EDD; sfxApply 0x934A8049 = level * EDE; 0xB580F7AB = level *
    EDF; weatherApply 0x965062BE = level * (EDE ? a weather global : 0),
    with EDC..EDF the four flag bytes (kFlags) and only when the mute gate
    is 0 and the mode gate is not; at level 0 it stops the scene; it zeroes
    the level every call. A second part does the same for 0x0E585610 (its
    own reference, variable 0xCA9ED31D) from the second level (kLevel2),
    which the second level's update writes every update;
  - the level is set to 1.0 by the scripted conversation manager's update
    when one of its conversations has a line under way (state 3, past the
    line's start), and the flag byte i is set when a playing conversation's
    current line has its byte +0x20+i (line +0x100); the four bytes are
    cleared every update.
Live, the scene is active with apply 1.0, weatherApply 1.0 and sfxApply
1.0 (so EDE was set) exactly in the recorded conversations, and never in
the replay: the mod's lines are not the manager's conversations, so the
level stays 0. The ambient speech, weather, ambience, shots and horse
categories fall 3 to 7 dB in those windows.


## Animal voice banks (src/animal_bank.h)

The speech manager (0x5BB9750, offline 1491.50) keeps one loaded bank per
animal type: 108 records of 0x48 bytes from +0x890, the record of type T
at +0x890 + T * 0x48 with its bank reference at +0x898 (the holder; +0x8A8
the reference's object, whose +0x20 is the bank slot entry), the type's
demand (float, +0x8B0), the next bank number to load (+0x8B4) and the
number of banks of the type (+0x8B4's modulus, +0x8B8). The manager's
update 0x2FC5E54 ends in 0x2FC65E0: with no load pending (+0x860/+0x870)
it asks 0x2F985B0 which type to load, finds a slot (0x2FA5C48: the horse
type 0x3B has its own slots, +0x850/+0x858; the others share the large
bank class +0x840/+0x848), builds the bank name "<TYPE>_NEAR_%.2u" from
the type's name (table at .data 0x3E276A0) and that number (0x2F9BD4C),
loads it (0x2FB1730: slot vtable +0x28, then the slot entry's +0x30 = the
joaat of the name, +0x38 = 0) and moves the number on ((n % count) + 1).
When the load completes, the type's holder takes the new slot.

The voice of an animal line IS that name's hash: 0x6D4D0CB7, 0xC66C3EF8
and 0x94C95BAF (horse voices) are joaat("HORSE_NEAR_01" ... "_03"). A line
asked with a voice (the native "pinned" path) gets the type's slot (voice
source 9, 0x2FBCE38 -> 0x2FBD004) and finds its wave only when that slot
holds the voice's bank; otherwise the sound sets +0xEE bit 2 and the line
dies loading (reason 4). A line asked by context gets the voice of the
loaded bank (0x2F9DB84 -> 0x2FA6674 reads the entry's +0x30 when its state
is 5). The manager rotates the bank when a context's variations were all
played from it (+0x38, 0x2FCEF8C) or when the type is not loaded, so a
replay's manager left alone plays recorded horses with the wrong voices.

## Conversation lines: sound set and duck byte (src/speech_conversation.h)

deeper the nearer the speaker is. Engine facts:
  - the sound set: the line's data +0xE8..+0xF7 is params +0xC0..+0xCF
    (copied by the line data init): +0xE8 the set object, +0xF0 its
    name hash, +0xF4 its metadata ref. The wave (name 0x75E4276D) and the
    five type 2 layers are built from it (the child ref builder: low dword
    the set's ref, high dword the index | 0x800000), so the set picks the
    line's sounds and with them their category. Params +0xC0 is written
    only by the SpeechParams applier (meta +0x14: +0xC8 = hash, the set
    metadata and its ref lookup give the ref -> +0xCC, the set object
    lookup the object -> +0xC0) and by the context applier (ctx +0x10, the
    same three calls), which fills it only while +0xC0 is still 0. The
    replay's ctor (the speech params ctor) applies no SpeechParams, so the
    context's set wins and a replayed scripted line picks the AI lines' set
    and category;
  - the duck byte: params +0x19E -> line +0x2BE bit 5. A d8 2 line of type
    1 or 2 with it raises every frame the speech duck level (a curve of the
    line's distance to the listener) that starts the mix scene 0x576520E1.
    Live the scripted lines had it clear (bits 0x02480000); the ctor sets
    it, so every replayed scripted line would duck its own voice (the
    ambient speech category, where the context's set put it).

## Speech wave slots (src/speech_slots.h)

Offline (build 1491.50):
  - the line's voice source (params +0xF8 -> line +0x218 in the line init)
    comes from the context metadata byte +0xD (the context applier,
    written only when +0xF8 is still 0); the camp chatter says 3 ("bank");
  - a source 3 line asks the bank slot getter with an EMPTY class
    mask (its constructor zeroes it): it only reuses a bank slot that
    already holds the bank of (voice, context) with a live reference
    (+0x10 != 0, +0x2C == bank). Nothing is loaded for it; the AI path
    preloads the bank before it says the line, the replay's direct path
    does not. No slot: the engine kills the line in the same frame (state 2,
    stop reason 4, line +0xD0 = 0);
  - a source 4 line ("ambient") asks the ambient slot getter: a
    free ambient slot (+0x10 == 0, +0x18 != 0) is loaded for the
    line's own wave, or the slot of the lowest real priority
    (priority x curve of the distance to the listener) is taken
    when the request's is higher. A source check turns a source 4 into 3
    when the bank is already resident, so the reuse then happens in
    the same call.

