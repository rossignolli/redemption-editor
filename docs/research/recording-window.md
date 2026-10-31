# Recording window: how each track is cut

Design notes behind `src/recording_window.h`, which cuts a recording to
`[fromMs, toMs]` starting at the base (the start of the lifecycle block that
holds `fromMs`). The code keeps the contract; the per-track rules live here.

## Tracks by how playback reads them

Each track is classified by how playback reads it (replay_events.cpp,
replay_objects.cpp, replay_ropes.cpp, pose playback):

```text
  Track               Kind       Playback reads it as           Before base
  ------------------  ---------  -----------------------------  -------------------------
  shots               one-shot   forward Range only             dropped
  speechLines         one-shot   forward Range (+ load ahead)   dropped (coreMs/stopMs shifted)
  footsteps           one-shot   forward Range only             dropped
  explosions          one-shot   forward Range only             dropped
  animAudio           one-shot   forward Range only             dropped
  fx                  one-shot   forward Range only             dropped
  meleeHits           one-shot   forward Range only             dropped
  bodyImpacts         one-shot   forward Range only             dropped
  sounds              one-shot   forward Range only             dropped
  collisionSounds     one-shot*  started only when its start    dropped when startMs < base
                                 is crossed in forward play     (*intensity samples cut to end)
  decals              state      initial pass (0,1] + reconcile folded to 1 ms (a)(b)
  decalRemoves        state      forward Range + chain removal  folded to 1 ms (b)
  projDecals          state      forward Range (persistent)     folded to 1 ms
  surfaces            state      latest per entity at playhead  last per entity, 1 ms
  gore                state      desired mask + forward Range   folded to 1 ms
  goreHits            state      desired mask + forward Range   folded to 1 ms
  blood               state      reconcile (aged) + forward     folded to 1 ms, age += gap (c)
  fireStarts          state      reconcile on the first tick    folded to 0, initial=true,
                                                                initialAge += gap (c)
  fireStops           state      reconcile + forward Range      folded to 0
  fireItems           samples    latest per fire at playhead    last per fire at 0
  fragBreaks          state      count of breaks <= playhead    folded to 0
  timeScale           state      latest sample at playhead      last at 0
  strapTracks         state      latest per weapon (else first) last per weapon at 0
  attachFrames        state      latest per weapon (else first) last per weapon at 0
  weaponTracks        samples    interpolated, clamped to span  last at 0 (d)
  gameCamera          samples    interpolated, held at ends     last at 0
  registeredFx        samples    frame at playhead, null after  track ended: dropped;
                                 endMs                          else last frame at 0
  ropes (times)       samples    InSpan / VerticesAt            record ended: dropped;
                                                                else last sample at 0
  ropes (held, slot)  state      latest (per slot) <= playhead  last (per slot) at 0
  lifecycle           blocks     BlockAt / AliveAt              blocks before base dropped
  created             creates    census and first create        entities of kept blocks only,
                                                                timeMs clamped to 0
  segments            state      latest started <= playhead     superseded ones dropped, 0
  pose records        chunks     chunk + frame at playhead      chunks before the base chunk
                                                                dropped; frames clamped to 0
  worldObjects        (no time)  stand-in until the prop        kept for kept props
  fragPieces          (no time)  the piece made at its creation kept for kept props
  scenery             life       appear <= t < gone             alive in the window, moved
  sceneryChildren     life       appear <= t < gone             alive with a kept parent, renumbered
  environment         clock      interpolated start to end      clock at base and at end
  environment.sky     samples    interpolated round the period  state at base, at 0
  environment.timecycle samples  keys held, floats interpolated state at base, at 0
  environment.moon    samples    interpolated round the turn    state at base, at 0
  place, area         (no time)                                 kept
  scorched            state      latest per entity at playhead  last per entity at 0
  vehicleWindows      state      latest per vehicle at playhead last per vehicle at 0
  vehicleParts        state      latest per vehicle at playhead last per vehicle at 0
  vehicleDoors        state      per door, interpolated         last per door at 0
  vehicleBurnt        state      latest per vehicle at playhead last per vehicle at 0
  burnLevels          state      latest per entity at playhead  last per entity at 0
  sceneryBurnLevels   state      latest per entry at playhead   kept entries, renumbered, last at 0
  objectLooks         state      latest per object at playhead  last per object at 0
  sceneryObjectLooks  state      latest per entry at playhead   kept entries, renumbered, last at 0
  fireChars           state      latest per entity at playhead  last per entity at 0
  appearances         state      latest per entity at playhead, last per entity at 0 when newer
                                 unless a later segment         than the segment at base
  world.ipls          state      latest per IPL at playhead     last per IPL at 0, "off" dropped
  world.entitySets    state      latest per set (else first)    last per set at 0
  spans               spans      span at playhead (editor)      part inside the window
  cuts                instants   no interpolation across one    after base only, shifted
```

## Notes on the table

- (a) The recorder puts the decals alive at record start at 1 ms and
    playback applies (0, 1] on its first tick; every forward cursor takes
    (from, to], so an event at 0 would never play. The state tracks that
    a forward cursor plays are therefore folded to 1 ms, the others to 0.
- (b) A decal add and its remove both before base are dropped together
    when the match is certain: the engine id appears in exactly one add
    (as its result, never as a request id) and exactly one remove, and the
    add comes first. Everything else is folded in order.
- (c) The age playback derives from the event time is kept: a blood record
    and a fire folded forward by `gap` ms are made `gap` ms older.
    A decal too (its settings aged, decal_state.h); one expired
    by then is dropped.
- (d) Kept whenever the entity is kept, even when its samples ended before
    base: the track clamps to its span, so the last sample is the state.

## Entity keyed tracks, the tail cut and session lists

Tracks keyed by an entity (weapon, strap and attach tracks, pose records,
segments, surfaces, decals, gore, blood, world objects, frag breaks) are
dropped with an entity that is not in the kept blocks. Everything after
toMs is dropped; a pose chunk that spans toMs loses its frames after it
(the frames are deltas on the earlier ones, so the tail cut is safe).
Pose frames before base stay in the base chunk (a delta frame cannot be
removed without re-encoding) with their time clamped to 0: the pose at 0
is the last frame at or before base.

Session lists (objectsBefore, pickupsBefore, adoptedObjects) and
fromDisk are copied as they are: a live buffer cut keeps its session.
