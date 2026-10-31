# Generic sounds: recording, playback and ownership

Design and engine notes behind the generic sound recorder and player (the SGEN
packets), the owner table that decides who plays a recorded sound, and the
rules that keep a puppet or vehicle from doubling them (RDR2 build 1491.50).
The code keeps the contract and a link here.

## Owner table (src/sound_owner_table.h)

One rule for who plays a recorded sound in the replay, so nothing plays
twice: the puppet by itself (the engine makes the sound when the puppet
animates), the specific track that already carries it (speech, footstep,
melee, shot, explosion, collision, entity sound, sound request, the census
capture), or the generic player (the SGEN packets). Never two.

A packet is given a family, in this order:
1. a specific track of the clip carries the same sound (same owner, same
   sound, within kCoverToleranceMs: the Cover index built from the clip's
   own tracks), so the family is that track's;
2. the sound set names it (the wet hoof layer set);
3. the family caller (the first return address outside the audio
   entity's wrappers, CreatePacket::caller) falls in a function of a known
   system (kCallerFunctions: the entries and helpers the mod's own detours
   and evidence logs already name, each function's extent in the game);
4. the audio entity's base (the weapon's audio: a shot);
5. the owner class (no owner: world; else owned).

Each family has a default taker and an ini override
(SoundGenericTake=family:taker;family:taker, takers puppet, specific,
generic). A family that already has a specific track keeps it; a family
the puppet's own engine update makes (the movement foley) is the puppet's;
the rest goes to the generic player. The generic player still yields at
run time to a sound the puppet (or the live world, or another track) makes
by itself around the packet's time (the anti-double rule of the capture,
sound_generic_player_core.h), which is the "puppet" taker decided per
sound instead of per family.

SoundGenericMode=packets (the default; legacy keeps the table above): a
replay entity sounds only what its recording carries, as a replay editor
usually works. The engine's own sounds of the replay's puppets are refused
at the creation ports (audio_observer.cpp: the persistent cores S05/S06 and
the deferred request S04 join the create-and-play refusal of S03), so every
SGEN packet with a recorded owner is the generic player's, with its
recorded adjusts, stops and release, and the tracks that replay a sound by
its ref (ESND, SNDS, CSND, GSND) leave a row to the generic player when a
packet carries it (PacketIndex below). The exceptions keep their track
(DefaultTaker with packets): speech (the recorder skips the line's sounds,
the speech track asks the line again), the footstep and wet hoof steps (the
footstep track replays the recorded foot event through the engine's own
step: material, wetness, audio LOD), and melee, gunshot and explosion
(their tracks replay the recorded event, whose sounds the engine makes
inside the replay's call along with the effects; their packets would
double them).

A sound is its set and entry (SoundKey), not its set alone: a horse's
surface sets are shared by the hoof steps (entries 5 and 6), a skid's
grains (entry 0x0E) and its body (entry 0x2B). The step exception covers
only the packets the step track remakes (FootWindows: near a recorded foot
event of that owner); the rest of the foot system (slide, drag, braking)
goes to the generic player.

## Vehicle and train car sounds (src/vehicle_sound_rule.h)

A vehicle's audio entity ([vehicle + 0x900]) creates at the creators
directly (e.g. sound 0x1056365 by creator B through the audio entity
wrapper, every 33 to 50 ms for a
rolling wagon), past every port the ped/weapon block watches
(soundowner::PortsBlockClass). In packets mode the generic player replays
the recorded packets and the puppet vehicle's own audio makes the same
sounds at the same call site, so they double and repeat.

The recording's packets own a vehicle's recorded sound: the puppet's own
vehicle audio alone measures about 9 to 11 dB under the recording, the
packets on the puppet's audio entity about 3 dB under; the own copy adds
at most 0.8 dB and is what repeats. So the peds' rule applies to vehicles
and train cars: an own create of a sound whose recorded packet is alive
then (Recorded) or of another set at that entry (Alternative) is refused;
the rest (Passed: the recording never had it then) is left to the puppet.
The refusal is the one the creator itself makes when its slot pool is
full (return 0, the out slot untouched; the wrapper then zeroes
it), behind the ini key SoundGenericVehicleBlock (default 1) because it
changes what a creator detour does.

Trains: the train cars' own vehicle audio makes nothing in a replay, so
the packets are the train's whole sound. The per-owner slot cap (6: a ped
controller's foley layers) would steal the cars' sounds and the 120 m
limit would refuse the locomotive; a wagon can have 13 recorded sounds
alive at once. Vehicles and train cars get their own cap, and train cars
their own audible distance (LimitsFor).

## Sound census: where it listens (src/sound_census_core.h)

Where it listens (static analysis): the two sound instance creators
(kCreatorA, kCreatorB) are the only callers of the requested-sound slot
allocator, so every top-level sound the game creates passes one of them.
Both take (sound manager, u64 metadata ref, audEntity*, audSound** out or
null, audSoundInitParams* or null) and return the sound (0 when refused).
Callers: the persistent cores by ref (41 sites) and by hash, the
create-and-play queue consumer (the queue's drain drains the one-shots the
queue push queued), and two more create entries.
Child sounds of a sound are made by the mixer from the parent's metadata and
are not seen here.
