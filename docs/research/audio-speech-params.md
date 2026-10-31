# Speech request parameters of the direct line entry

Offline notes (RDR2 build 1491.50) behind `PlaySpeechDirect` in
src/audio_observer.h: which speech params fields the replay writes and where
the engine carries them.

## PlaySpeechDirect (src/audio_observer.h)

Replays a recorded line on a ped: the recorded context and variation
through the direct line entry, without the AI Say gating, and no voice
in the request (the voice is pinned on the holder, PinSpeechVoice).
`result` is the core's code (74 = line queued). With `expected`, the
created line is compared with it before the start; on any difference
the line is stopped instead of started (silence rather than another
phrase). Only params +0x18C is set (skips the recently-played check,
which otherwise stops every replayed line in state 3); +0x18D is left 0
(it only enables the variation re-draw).
`startOffsetMs` > 0 asks the line from that point (speech_resume.h;
speech params +0x15C; offline: the params constructor writes 0
there, both params copies keep it, the line data init (kLineDataInit)
moves it to data +0x11C, the speech sound's creation and the line's two
sound starts put it in the init params +0xE8, which creator A
(census::kCreatorA) reads as the
start offset, beside the predelay +0xEC).
`source` > 0 is written into params +0xF8 (speech_slots.h; 4: an ambient
slot by priority, the line's own wave).
`ask` (may be null, speech_request.h) writes the recorded line type into
params +0xF4 (the ctor leaves 0 and the params validator writes 2 for
0) and the recorded request fields (volume type +0xDC, audibility +0xE0,
+0xD8, +0xE8), after the ctor. An Ask with a sound set
(speech_conversation.h) writes params +0xC8 (hash), +0xCC (ref) and
+0xC0 (the set object) as the SpeechParams applier (kSetApplier) does, the
object found with its own calls (kSetMetadata, kSetRefByHash by hash,
kSetObject by ref); the outcome goes to played.setStatus / setWrittenRef.
