# Weather, wind, moon and timecycle

Reverse engineering notes behind the environment tracks of the replay: the
audio wind, the moon angles, the weather transition and script overrides, and
the timecycle modifier with the weather variations. RDR2 build 1491.50,
offline dump unless stated otherwise. The code keeps the contract and a link
here.

## Audio wind in the replay (src/wind_replay_core.h)

Engine facts (the code below, found by signature):
- The weather audio entity W (a static object; its initializer stores its
  vtable) pre-updates through its vtable slot +0x28 (kPreUpdate). It reads
  the local player (the obfuscated cells), his position (+0x70), and asks
  the map data box streamer whether the world is loaded about that
  position: the gate (kGate: streamer, &pos, 0x78), called at kGateCall.
  On false (the je after the call), or with the fade >= 0.9, or another
  global != 0, it takes the SIMPLE path (kSimple): W+0x50A4 = time and the
  wind update with the fifth argument 1 (kSimpleUpdate), which sets the
  speaker levels of the wind slots to {1,1,1,1} and never calls the
  function that swaps the wind slots to the wind sounds of the current
  weather when it changes (kLines: it compares the weather index each row
  was started with, W+0x5094 / W+0x5098, with the previous / next weather
  index and stores the new one).
- In a mission replay the live player stays hidden where he was (the
  follow is skipped) while the streaming focus goes to the replay camera,
  possibly hundreds of metres away: the streamer unloads his area, the
  gate answers false and the replay's wind freezes on the slots started
  in its first frames, at full level on the four speakers, whatever the
  recorded weather does next.
- wind_str is not an input: it is the curve (block+0x3560, hash
  0x87AA362D) of the peak meter of the "Wind" submix, smoothed by 0.05 and
  stored by kStrength: the measured loudness of the wind bus itself.

The fix: while the replay owns the world, the weather gate's question is
asked about the recorded scene instead of the hidden live player. A detour
of the gate (prologue checked) forwards every call, and only for the call
from the weather pre-update (its return address, kGateCall + 5) during
playback it passes the replay's streaming focus (live, the player is the
engine's focus entity), else the rendered camera, else the trail focus
puppet. Same streamer, same mask: the engine answers whether the world is
loaded there and computes the wind as it did live. No wind value is
written.

Evidence (clip section WNDA, optional): the recording keeps, 4 times a
second, the audio wind strength (W+0x4F44), the weather each wind row plays
(W+0x5094/W+0x5098 through the weather table) and whether the simple path
ran (W+0x50A4 moved). The playback reads the same and logs the recorded
value against the one the engine computed (WIND_REPLAY, every 5 s of clip
time, and WIND_REPLAY_TOTAL). A clip without the section still gets the
fix; its lines say rec=none.

## Moon angles (src/moon_track.h)

```text
Engine model (build 1491.50, offline dump; the sky object and its
update, sun_control.cpp):
  delta    a global integer, written by the clock step every frame: the
           game clock in seconds ((days since 1898 * 24 + hour) * 60 +
           minute) * 60 + second, minus the previous frame's. A jump of the
           clock (SET_CLOCK_TIME, a trainer, the replay itself) or of the
           date is one frame with a large delta; a paused clock is 0.
  +0xA80   double, the moon's turn angle. Only when the sun (+0x50) and the
           moon (+0x260) of the previous frame are both below +0x8CC
           (-0.15, z) the update adds 2pi * delta / (+0x8D0 (2.5) * 3600)
           and wraps with fmod by 2pi (up to the angle's store); otherwise it
           only reads it. It enters the moon's daily rotation
           matrix +0xA40 next to the clock's own term.
  +0x8E8   double, the moon's orbit angle. Every frame (no condition) the
           update adds delta * 2pi / 86400 / +0x8C0 (27.322, the sidereal
           month in days) and wraps it the same way (up to the orbit's
           store); with the tilt +0x8C4 (28.58 degrees) it builds the
           matrix +0x940. The drawn moon +0xAA0 is a constant vector times
           +0x940, +0xA40 and +0xA00; the sun +0xA90 uses none of them.
  The constructor zeroes both; no other reader or writer
  of the two fields was found (no rip-relative reference to them, none in
  the functions handed the object). So both are the integral of the game
  time the session has run (the date included), not saved anywhere: the
  moon's place and its angle to the sun (its phase, if the renderer lights
  it from the two directions: not proven) are session state.
```

### What the replay does with them

```text
What the replay does with it (script.cpp):
  1. A clip that sets the clock (environment recorded) saves both angles
     when it opens and writes them back for a few frames after the clock is
     given back (the jump back consumed), so the replay's own clock jumps
     never move the session's moon. Also after a script reload (the
     world-restore file) and at unload.
  2. The recording keeps both angles (4 Hz, on change), with the drawn sun
     and moon and the clock date as evidence (section MOON). Playback with
     the Moon phase row Recorded writes the angles at the playhead every
     tick from the script fiber: with the clock paused (delta 0) the write
     holds, and the update itself computes the moon, the shadows and the sky
     from the recorded angles and the clip's clock. A clock jump (scrub)
     costs at most a frame before the next write.
```

## Weather transition and script overrides (src/weather_state.h)

```text
(environment_track.h). Rung 2 only: guarded reads and writes of the weather
manager's transition fields, and guarded reads of the script overrides of
wind, rain and snow (to give them back), at addresses taken from the game's
own code after a signature check (build 1491.50):
  GET_CURR_WEATHER_STATE handler: lea r10, [the weather table] (the
            manager W), types at W + 0x10 + i*0x1E0 (hash first),
            previous index at W+0x3F24.
  the transition setter (SET_WEATHER_TYPE with a time, via its handler
            and core): index < [W] (the type count), time * the
            constant 1000.0 (milliseconds), W+0x3F44 progress = 0,
            W+0x3F38 target index, W+0x4170 flag byte, W+0x3F3C total,
            W+0x3F40 time left.
  the per-frame update: time left -= frame time * 1000, progress
            = 1 - min(left / total, 1); at 1 it calls the finalizer with
            lea rcx, [W] (the target becomes the weather) and clears the
            five fields (target -1).
  SET_RAIN handler: movss [W+0x4160], xmm0;
            _SET_SNOW_LEVEL handler: movss [W+0x4164], xmm0. The level
            readers (rain, snow) use the override when it is >= the
            constant 0.0; the manager starts them at -0.01 (its init): a
            negative value is "not forced".
  SET_WIND_SPEED: below the constant 0.0 clears the wind object's (V)
            flag byte; otherwise lea rcx, [V] -> the speed setter:
            speed at V+0x60, flag byte V+0x68.
  SET_WIND_DIRECTION: below 0.0 clears [V+0x80]; otherwise
            lea rcx, [V] -> the vector maker (degrees * the constant pi/180,
            vector (-sin, -cos, 0)) stored at V+0x70, flag byte V+0x80.
A mismatch disables the transition and the overrides (logged once by the
caller); nothing is guessed. Only the script thread calls these. That the
weather update (called from one site only, in a function itself called
from one function only) runs on the same game thread, between script
ticks, is a hypothesis (the chain up to the thread was not followed):
WriteTransition orders its writes so a frame in between stays sane.
```

## Timecycle modifier and weather variations: engine model (src/timecycle_track.h)

```text
Engine model (build 1491.50; the engine side is timecycle_state.cpp):
  timecycle object T: T+0x2770 modifier index, +0x2774 strength,
    +0x2778 transition index, +0x277C transition progress, +0x2780 speed
    (progress per second), +0x2784 fade-out byte. The per frame update
    adds speed * frame time to the progress of a transition and
    finishes it (the target becomes the modifier) when progress reaches the
    strength; with the fade-out byte it takes speed * frame time off and
    clears the modifier at 0.
  An index is a position in T's modifier pointer array (T+0x40, count at
    T+0x48). SET_TIMECYCLE_MODIFIER finds it from the name hash through a
    sorted map of (hash, pointer) at T+0x50 (data at +0x58, count at
    +0x60) and the position of that pointer in the array (its index lookup).
  Weather type i at W + i * 0x1E0: +0x10 its hash, +0x14 the key of the
    active variation (the type's own hash, or 0, when none), the variation
    table at +0x1C8 (entries of 0x44 bytes, key first), count at +0x1D0.
```

## Timecycle and variation code sites (src/timecycle_state.h)

```text
The engine side of the timecycle track (timecycle_track.h). Guarded reads and
writes of the script timecycle modifier and of the weather types' active
variations, at addresses taken from the game's own code after a signature
check (build 1491.50):
  SET_TIMECYCLE_MODIFIER handler: jmp to the setter
  the setter: lea rcx, [the timecycle] (T); call the index of the name hash;
            or [T+0x2778], -1; mov [T+0x2770], eax
  the index of a hash: the alias lookup (alias at T+0x18, count T+0x38,
            then the sorted map at T+0x50) and the position of the
            pointer in the array at T+0x40 (u16 count at T+0x48)
  the sorted map's find: data at +0x8, u16 count at +0x10, 16-byte
            entries (u32 key, u64 pointer at +0x8)
  the per frame update: +0x2780 * frame time into +0x277C,
            against +0x2774; +0x2778 into +0x2770; the byte at +0x2784
  CLEAR_TIMECYCLE_MODIFIER handler: jmp to the clear (index and
            transition -1, strength = the constant 1.0, byte +0x2784 = 0)
  _SET_WEATHER_VARIATION handler: jmp to the implementation
  the implementation: lea rcx, [the weather table] (W); the type by hash
            (u32 count at W, hash at W + i * 0x1E0 + 0x10); the variation
            by name -> by key (table at +0x1C8, u16 count at +0x1D0, 0x44
            bytes an entry); call the variation setter(W, type, variation, 0)
  the variation setter: writes the entry's key to W + type * 0x1E0 + 0x14
            (and takes the type off the pending clear list at W+0x3E38);
            a negative variation goes to the variation clear (immediate
            with the 4th argument: +0x14 = +0x10)
A mismatch disables the track (logged once); nothing is guessed. Reads and
writes happen on the script thread, as the natives above do (no detour).
```
