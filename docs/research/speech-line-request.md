# Speech line requests

Reverse engineering notes behind the code that records a speech line's request
and asks it again in the replay: the request fields, the line data types, the
scripted (conversation) path and the player's own lines. Build 1491.50, offline
dump unless a note says it was measured in game. The code keeps the contract
and links here.

## Request fields and line data types (src/speech_request.h)

A line the ped's own AI said when recorded (path=ai, data_type=1, e.g. the
gang's camp chatter, context 0xe9a2985b) comes back on the replay's direct
path as data type 2 unless the type is asked: the direct entry builds its
params with the ctor (which zeroes +0xF4) and the human validator writes
2 when +0xF4 is 0. Offline (build 1491.50):
  - params +0xF4 picks the line data class (the data allocator's switch,
    copied to data +0xE0 by the data init). Type 1 (0x158 bytes) plays ONE
    sound, the wave (data +0xC0): its play setup puts the
    request's volume, pitch and the two distance values straight on it,
    and its per-frame volume takes component [2] of the line's
    volume vector. Type 2 (0x1D0 bytes) plays the wave as a
    parent (the volume setter gets -100 on it) plus up to five child sounds of
    a speech sound set (names 0x75E4276D, 0x623B8117, 0x52F58009,
    0x0FDF5A1C, 0xBEE1D782 on data +0xE8, created by the layer sound
    create as layers 0, 4, 1, 2, 3), adds a metadata volume (in mB)
    to the request's volume, and feeds each layer its own component
    [0..4] (its per-frame volume);
  - the volume vector: a positioned line gets the
    volume on [2], [0] and [1] silent (-100 dB), [3] silent unless params
    +0xD8 is 2 with the type 2 pad flag, and [4] the share a crossfade
    moves off [2] (silent by default); a volume type 2 line
    (frontend, no position) gets it on [0] with [2] silent. Type 1 hears
    [2] only: the layers and the crossfaded share that type 2 adds are the
    difference (its audible size depends on runtime metadata, not proven);
  - the request fields the engine copies to the line when it creates it
    (the line init): +0xD8 -> line +0x214, +0xDC -> +0x21C, +0xE0 ->
    +0x220, +0xE8 -> +0x224, +0xF8 -> +0x218, +0x100 -> +0x228, +0x104 ->
    +0x22C, +0x108 -> +0x230, +0xEC -> +0x23C, +0xF0 -> +0x240, +0x168 ->
    +0x27C, +0xD4 -> +0x1F8, +0x170 -> +0x290, +0x178 -> +0x294, +0x180
    -> +0x2A4, flag bytes -> +0x2BC..+0x2BF. The SpeechParams applier
    (metadata type 0x12, the public speechParamHash) writes
    +0xDC from its RequestedVolume byte (+0xC) and +0xE0 from its
    Audibility byte (+0xD), +0xD8, +0xE8 and +0x104 from three more bytes,
    +0x168 from its preload timeout (default 30000 = 0x7530 in the ctor);
    the volume vector adds a dB offset by audibility (a table: -1.5,
    -0.75, 0, 0) and by volume type (a table: 0, 0, 0, +3), and
    the engine picks the distance values of the play setup from the tables
    of +0xE8 (1 to 3; 0 and 4 none) indexed by +0xDC and +0xE0. The
    replay's ctor leaves +0xD8 = +0xDC = +0xE0 = 0 and +0xE8 = 4.
The recorder reads the line's copies of the request (what the engine really
used, after its own resolution) and the replay writes the line type and the
four request fields the SpeechParams metadata sets, each only in the range
the engine reads (the volume vector refuses +0x21C or +0x220 >= 4, and
+0x214 >= 5).
The other fields are only compared: one diff line per line type and a count
per field.

## Scripted (conversation) lines and the player's lines (src/speech_script_path.h)

A scripted line (a campfire story, a mission conversation) is recorded with
source 2, pD4 0xF and line bits 0x02480000; asked as an ambient line it
comes back with source 4, pD4 0 and bits 0x02688080, the ambient sound and
3 to 4 dB lower at the same distance. Engine facts (build 1491.50):
  - the slot allocator by source switches on line +0x218: source 2 goes
    to the script slot pick, which takes the first free one of the speech
    manager's 24 script slots (stride 0x28 at the manager; the slot free
    test, slot vtable +0x20) and loads the line's own wave in it:
    nothing to preload, no bank, independent of the ambient slots the
    replay manages (speech_slots.h);
  - pD4 (params +0xD4 -> line +0x1F8) is a 4 bit mask the state 3 rules
    (the other speakers' rule) match against the other lines'
    kind before refusing an overlap; 0xF lets the line coexist with all;
  - line +0x2BE bit 5 comes from params byte +0x19E (the line init);
    the per-line duck, called by the line's per-frame update,
    sees it on a d8 == 2 line of type 1 or 2 and every frame
    raises the global speech ducking flags and level
    that eight audio engine functions read (the duck driver among
    them; what each one ducks is not identified). The recorded
    scripted lines had it clear, the replayed ones set (the params
    default), so a replayed scripted line raised a duck the live one did
    not;
  - line +0x2BD bit 7 (params +0x130 != 0) is read only behind the network
    game flag: inert in story mode, not
    asked again.
So a line recorded with source 2 is asked with source 2, the recorded pD4
and the recorded bit 5 of +0x2BE, and skips the ambient slot gate.

The player's scripted line: live, the player's scripted lines have the
engine distance d_lst = 1.0 whatever the camera distance (measured 2 to
10.5 m), the NPCs' d_lst ~ d_cam - 1; in the replay the hidden player says them (SpeechPlayerOnPlayer) at the
body, 4 to 9 m from the camera. With SpeechPlayerScriptNear the puppet
says them, and while the replay renders the recorded game camera (the
rendered camera within kNearGameCameraM of it) with the frontend volume
type 2 (the volume vector: no position, the volume on component [0]), which is
what "at 1 m from the listener" sounds like; farther (a free camera) the
puppet says it positioned at itself. That the live 1.0 m is a listener
rule of the player's speech is a hypothesis (the d_lst reading itself is
one, mix_probe.h).

## Speech rules the replay bypasses (src/speech_replay_rules.h)

A replayed line can die in state 3 with stop reason 6
(POSE_V2_SPEECH_SILENT cause=revalidation) with the code of the speech
entity's rules (vtable +0x68, SpeechState3Rules for a ped; the state 3
handler stops the line when the code is below 0x46, a failure):
  - 28 (0x1C, the overlap rule): another speaker of the manager's
    list (+0x78, count +0x80) is saying a line whose data priority (+0x25
    of [line +0x68]) is at least this line's;
  - 41 (0x29, the range rule): the speaker is farther from the listener than
    the line's range mode allows (35, 60 or 100 m), unless line
    +0x34 is 1 (e.g. away from the replay camera);
  - 48 (0x30, the conversation rule): a scripted conversation is going
    on (a global u16, which a script native reads as "> 0") or its time
    stamp is not past, and neither the line's speaker nor its target
    (+0x78) is the player ped, unless line +0x38 is set.
The three judge the world around the line (other speakers, the listener,
the conversation state), which in a replay is the replay's own: live the
recorded line passed them. A replay says a recorded line without the AI's
can-say rules. So with SpeechReplayRules=1 (default) the rules detour
(world_theatre.h kHookRules, forwarded first) gives "accepted" (0x4A) for
these three codes on a line the replay asked; any other code (18: the
speaker's own newest line of the context, 0x23: no ped, 0x2F: dead, ...)
stays as the engine said, and so does every world line.

## What the speech level probe reads (src/speech_level_core.h)

What is read (dump 1491.50, guarded reads):
  - the ped's speech entity lines (+0x78 list, u16 count +0x80), state
    +0x234, data +0x1D8 (type +0xE0, voice +0x110, context +0x114), source
    +0x218, d8 +0x214, volume type +0x21C, audibility +0x220, bits +0x2BC
    (speech_request.h);
  - type 1 lines: the per-frame volume (the line data's vfunc,
    speech_request.h) smooths component [2] of the line's volume vector
    through the smoother on data +0x148 (rate +0, value +4, time +8, flags
    +0xC bit 0 = running) and writes the result back: data +0x14C is the
    line's volume as the engine applies it (linear; converted from the
    dB of the vector). Type 2 lines feed five layers (their per-frame
    volume): no single value, logged as "na".

## Why the hidden player refuses conversation lines with 68 (src/speech_player_route.h)

Why the hidden player refuses the recorded player's conversation lines with
68 (build 1491.50):
  - the variation step checks the line against params +0x14C
    (voice name hash), +0x150 (context) and +0x158 (variation): with a
    variation it calls a variation lookup (voice, context, variation), and
    with the replay's +0x18C set it draws a variation again (voice,
    context); no variation of that context in that voice is the 68;
  - the voice step keeps any non-zero +0x14C; with none it
    comes from the speaker's voice holder (its resolver: holder
    +0x30, else a draw from the model's voice group);
  - the replay pins the recorded voice on the PUPPET's holder and asks
    with no voice, so on the hidden player +0x14C is the player's own
    current voice, which may not have the line.

## Pain vocals on the hidden player and on the puppet: evidence (src/speech_player_route.h)

Said by the hidden player they play (live evidence, 1268-Blackwater: 9 of 9
started, heard at -13.8 dB). Said by the puppet they die while loading
(stop reason 4 in state 2, cause killed_loading, QA-232-Valentine and
pp-1261-Valentine-p7): the puppet has no wave for the player's pain voice,
so the line is silent or, through the own voice retry, another voice.

