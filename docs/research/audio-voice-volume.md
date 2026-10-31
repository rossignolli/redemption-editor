# Audio voice volume and the mixing chain

Reverse engineering notes behind the volume probe and the code that reads the
audio engine's per-voice volume stages (RDR2 build 1491.50, offline dump). The
code keeps only the contract and a link here.

Addresses are RVAs of the game image unless written as a bracketed global
(`[0x...]`, a pointer read from that RVA). The addresses in parentheses are
the instructions that prove each fact.

## Volume probe (src/volume_probe_core.h)

The probe reads the bucket's voice list (the list the audio frame
processes), not the parent bitmap, so every playing voice is seen, not only
the top-level environment sounds (the speech lines). The voices of the
weather, ambience, emitter, particle, script and music entities get classes
of their own; the residual of the chain is split into the mix group term and
the mixer scene term the engine applies (what is left is occlusion plus
distance attenuation); quad speaker levels (the unpositioned beds: wind) are
read; and the controls suspected of the loud wind are logged per window
(VOLUME_PROBE_ENV).

### Engine layout

Every site below is checked byte for byte at runtime before the first read
(`kSites` in the code).

```text

Sound pool (the pool the requested-settings processor 0x2533EF0 and the
audio frame 0x2539D5C walk):
  [0x53588D8] pool table; bucket b at table + b * 0x2A860 (0x2539F91,
  0x258141F), buckets
  [0x53588B8] (0x253A0DF cmp), slots per bucket [0x53588C0] (0x2539FC6),
  sound slot = [bucket+0x2A848] + slot * [0x53588B0] (0x25326EB, 0x2539FB3),
  requested settings block = [bucket+0x2A850] + index * [0x53588B4]
  (0x2533FB9, 0x2533F9C).
  bucket+0x2A440: one PARENT bit per slot (set only by the factory
  0x252AAE0 at 0x252AC1B for a sound without a parent; 0x25326A0, which
  reads it at 0x2532768, is the parent iterator). It holds only top-level
  sounds.
  bucket+0x2A846 u16: entries of the bucket's voice list (zeroed by the
  frame at 0x2539FAA, bumped by 0x2581400 at 0x2581443, at most 0x200
  0x258141A); bucket+0x29840 + i*2 u16: the environment sound's slot of
  entry i (0x258149D, 0xFFFF a removed entry, read by the frame's iterator
  0x25325C4 at 0x25325D4 / 0x25325ED / 0x2532602); bucket+0x24040 + i*0x2C:
  the combine buffer of entry i (copied at 0x258145C; the frame passes it
  to 0x256C3EC at 0x253A0A0/0x253A0A4): +0 the combined volume of the
  hierarchy in dB, +4 its post-submix dB (0x256C730, 0x256C738).
audSound (any type): +0xA0 bucket byte (0x253270C), +0xE2 parent slot
  (0xFFFF none, 0x25326FC), +0xD6 requested settings index of a top-level
  sound (0x2533F87), +0x48 tracker and +0x50 audio entity of a top-level
  sound (0x2585DEF, 0x2585DA3).
audEnvironmentSound (vtable 0x360E208; its update 0x2567D18 at slot +0x50
  registers it in the voice list):
  +0x240 u16 its voice index (0x2567D88; 0xFFFF once freed, 0x2573810),
         +0x242 u16 its voice list entry: written by the registration
         (0x2567FF9) and set back to 0xFFFF by the frame's metric builder
         right after the voice is mixed (0x256D831 / 0x256D896, rdi = E,
         r15d = 0xFFFF at 0x256D825; skipped only with r8b = 0, a lone
         query 0x253A18C; the frame passes r8b = 1 at 0x253A09D).
         Between two frames every mixed voice holds 0xFFFF there, so the
         probe keeps an entry when E+0x242 is i (registered, not mixed yet)
         or 0xFFFF (mixed), and refuses another index (backlink_bad);
  +0xD8 u16 the requested settings index of its top sound, read straight
         from the leaf (0x2568075; the metric builder 0x256C480 uses it);
  +0x224 f32 the voice's final linear gain of this audio frame (0x256D644):
         volume * (dry + wet) * chain;
  +0x238 s16 category index (0x256CA3D) into the resolved category table
         [0x5338488] (0x256CA51), entries of 0x48 bytes, +0 the category
         volume in dB (0x256CC65);
  +0x249 & 0x1F the voice's bus: with 0x25313D0(bus) true (0x11..0x20,
         0x22, 0x23) the post-submix, the category and the mix groups are
         NOT applied (0x256C747 zeroes the post, 0x256CC5B, 0x256CD60,
         0x256CEBE skip);
  +0x1A0 6 current mix group bytes, +0x1A6 the 6 previous ones (0xFF none,
         0x256CCC9, 0x256CD79), each group slot j fading by the byte
         S+0x64+6j / 255 (the metric copy of S+0x50 at rbp+0xB0, 0x256C889,
         0x256C88D; 0x256CCB1, 0x256CE17): current group factor
         1 + f*(v-1), previous group factor v + f*(1-v), v the resolved
         volume of the group's patched category (group pool [0x5338558]
         0x256CD93: +0x10 count, +0x38 used bits, record = [+8] + g*[+0x20],
         record+8 the s16 category remap table, 0x256CDD0..0x256CDDF);
  +0x134 u16 ids of the mixer scenes matched by the sound, count at +0x19C
         (0x256CFB5, 0x256CFCB), filtered by the 6 s16 tags at +0x1AC
         (0x256D03D): scene by id in the lists [0x53479D0]/[0x53479D8] and
         [0x5347900]/[0x5347908] (0x252909C), +0x2008 id, +0x2000 tag (-1
         matches all, 0x25316D0), +0x1FFC mode (0 multiply, 1 minimum,
         0x256D168), +0 the linear volume (0x256D1A5) multiplied into the
         chain (0x256D21A);
  +0x108 the voice state V (0x256D600): V+0xD0 the combined volume made
         linear after a -2 dB bias, V+0xD4 the chain: post-submix * category
         * mix groups * scenes * occlusion * distance attenuation.
Requested settings block S (double-buffered by the game index [0x533852C]
  and the audio index [0x5338528], 0x256F4CB): requested volume dB
  +0x120 + idx*0x40 (0x256F50B), post-submix dB +0x124 + idx*0x40
  (0x256F514), dry +0x12C and wet +0x128 + idx*0x40 (0x256CAEC, 0x256CAFE),
  position +0x10 + idx*0x10 (0x256C58F, 0x256C784); the position mode in
  bits 2..3 of +0x25F (0x2536F7C/0x2536F83: 0 = no position); quad speaker
  levels: SetRequestedQuadSpeakerLevels 0x2536ED4 writes 4 linear levels
  into the position slot (0x2536EE8), clears the position mode
  (0x2536EFE) and sets bit 0x8000 of +0x14C + idx*0x40 (0x2536F0F): an
  unpositioned bed (the wind slots, 0x31C6D30); occlusion "open" S+0xD4,
  environment group S+0x248.

Global audio entities (static objects of the image; the probe checks the
first qword against the vtable the constructor writes): weather
0x5C90070 (vtable 0x383D3B0, static init 0x112D90/0x112D9C), ambience
0x5CA7BB0 (0x383F010, 0x1154E8 + ctor 0x31FA632; its 0x3200D20 loads
AMBIENT_ZONE_LIST), static emitters 0x5C505C0 (0x3832418, 0x10D8E4 + ctor
0x3100620; SET_STATIC_EMITTER_ENABLED), particle emitters 0x5C9A660
(0x383ECF0, 0x11555A/0x115566), script audio 0x5C81C20 (0x383D4A8,
0x112D24 + ctor 0x31B997D; SET_AUDIO_FLAG, START_AUDIO_SCENE,
SET_AMBIENT_ZONE_STATE), music manager [0x5C7D2B8] (heap, vtable
0x3837D48, ctor 0x316A4F7; AUDIO_IS_MUSIC_PLAYING 0xF5E3FD reads it; its
two track slots at +0x6A8 + k*0x390 hold the sound at +8, 0x316FB78,
0x316FB7F, 0x316FB88).
Weather entity W: the wind block at W+0x1980 (0x31B874B), its 12 sound
  slots (2 x 6) at block+0xF90 + i*0x130 with the sound at +0 (0x31CF136,
  0x31CF141, 0x31CF145, 0x31CF14B, 0x31CF1AE); wind strength smoothed
  block+0x35C4 = W+0x4F44 (0x31CCC36); script wind type override W+0x50A8
  with the hashes W+0x509C/W+0x50A0 (0x31B8846, 0x31B884F, 0x31B8864); the
  wind variable 0x1C6CE2B3 override W+0x508C with its value W+0x5074
  (0x31BDC88, 0x31BDC91); the weather mixer scene W+0x5068 (0x31CEF79);
  weather types: indices [0x4576204] (previous) and [0x4576208] (next) in
  the table 0x45722E0, stride 0x1E0, hash at +0x10 (0x31B8737..0x31B8759).
Interior: 0x5B83300 (0x31CCC13), in interior byte +0x343D (0x31D73ED),
  interior ratio +0x3444 (0x31D7403).
Exposure per direction (4 floats): 0x5C97790+0xBF4+dir*4 (0x31C6E7E,
  0x31E1D9F); multiplied into the wind slots' levels.
Speech duck (the conversation self-duck the speech analysis found):
  level 0x5B5BF24 (0x2FCC2D9), flag 0x5B82EDE (0x2F2D300), mixer scene
  pointer 0x5BA5FE0 (0x2F2D255).
Audio flags (SET_AUDIO_FLAG): 88 counters at 0x5C81C20+0xCE6C+4*i
  (0x31C62BF, 0x31C6457; index 65 OpenWorldMusicDisabled read at
  0x317391D), names hashed in the table 0x383CDE0..0x383CF40 (0x31C6243,
  0x31C6251).
The listener: the game's audio environment volume points
  (game_listener_core.h, already proven).
Speech: a line's data (line +0x1D8) holds its sounds at +0xC0 (0x2FA10C0)
  and the layers at +0x198 + i*8 (0x2FA0DB9).
```

## Volume probe, engine side (src/volume_probe.h)

The volume probe, engine side. A measurement build: in the recording and in
the replay, every VolumeProbeSampleMs of wall time it walks the engine's
sound pool, reads every audEnvironmentSound (one per voice that plays) and
splits its final gain into the engine's stages (requested and combined
volume, post-submix, resolved category volume, mix groups + occlusion +
distance, distance to the volume listener), classifies the voice by what
made its top-level sound (speech line type/source, census caller family,
recorded owner) and sums it per class and per engine category over windows
of VolumeProbeWindowMs of clip time.

Engine access: rung 2 only (guarded reads of fields proven offline,
volume_probe_core.h kSites, checked byte for byte once; a mismatch disables
the probe and logs VOLUME_PROBE_CHECK,ok=0,site=...), plus natives that read
(positions, the rendered camera, AUDIO_IS_MUSIC_PLAYING). No detour, no call
into the engine, no write. Script thread only. Every read refuses an address
below 4 GB without touching it (guarded_memory.h kGameHeapLowest), and the
bucket stride is a site. post_db is the post the chain applied (the combine
buffer's, 0 on a skipping bus), the speech sounds are read right before each
snapshot, born rows age out, a voice list that moves under the walk is
walked again (list_retried, list_growing), and a pass ends with its
recording or playback.

The walk follows each bucket's voice list (the list the audio frame
processes: every voice, not only the top-level environment sounds). An
entry of the voice list is kept when its sound's E+0x242 is the entry
(registered, not mixed yet) or 0xFFFF (mixed: the frame's metric builder
resets it, 0x256D831); a sound slot counts once per bucket walk (list_dup).
The global audio entities' voices get classes of their own (wind, weather,
ambient, emitter, ptfx, script, music; hoof and horse from the census); the
chain's residual is split into the mix group term and the mixer scene term
(rest = occlusion + distance); the quad speaker levels of the unpositioned
beds are read, and the controls of the wind and the speech duck (with its
weatherApply and sfxApply) logged per window. quad_index, env_group and
posmode_set are read off the prologues the sound port detours hook.

Lines: VOLUME_PROBE_CHECK (once; ok=0,part=... turns one optional part
off), VOLUME_PROBE (per closed window, one per class with voices and one
"all"), VOLUME_PROBE_CATEGORY (per window, the loudest
VolumeProbeCategories categories), VOLUME_PROBE_WORLD (per window, the
loudest VolumeProbeWorld groups of world-side voices by entity, top type
and category), VOLUME_PROBE_STATE (per window: snapshots, voices,
listener, music, sync byte, cost, the walk's evidence), VOLUME_PROBE_ENV
(with VolumeProbeEnv=1: weather and wind controls, interior, exposure,
speech duck, audio flags, music, mixer scenes below unity),
VOLUME_PROBE_TOP (with VolumeProbeTop=1: the loudest voice of each class
per window), VOLUME_PROBE_TOTAL (per pass). Every window line carries
seg= (a new segment after a loop or seek back). tools/volume_probe_diff.py
reads them.

## Derived stages: which post-submix the chain applies (src/volume_probe_core.h)

Derived stages (dB). final ~= volume + wetdry + chain; resid = chain -
post - category (both 0 on a skipping bus) = mix groups + scenes +
occlusion + distance attenuation; rest = resid - groups - scenes.

The post the chain multiplies is
the COMBINE BUFFER's (+4: the requested post-submix S+0x124 plus what
the hierarchy adds, 0x256F514/0x256F58B), not S+0x124 alone: the
metric builder loads it at 0x256C738 (movss xmm8,[r9+4], r9 the
combine the frame passes), makes it linear at 0x256C861..0x256C879
([rbp+0x19C]) and the category multiplies that (0x256CC86); S+0x124
is not read in 0x256C3EC at all. Subtracting S+0x124 would put a
post-submix attenuation of the sound's metadata (or a different one on
the sound the replay asks) into resid_db and rest_db, read as
occlusion or distance. AppliedPostDb is the post the engine applied
(0 on a skipping bus, 0x256C762); AppliedCatDb the category (0 on a
skipping bus, 0x256CC5B).
