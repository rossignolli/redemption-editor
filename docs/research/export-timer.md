# Export timer: locking the game clock to the export clock

Notes behind `src/export_timer.h` (RDR2 build 1491.50, offline dump).

## How the lock works

```text
How (rung 5 of the engine access ladder, build 1491.50):
  fwTimer::Update (found by the signature of its prologue) computes
    realDelta = float(QPC() - [previous counter]) * [seconds per tick]
  and then stores a second QPC() into [previous counter]; every time set is
  advanced by realDelta (times the warp: Dead Eye, SET_TIME_SCALE and the
  other slots still apply on top, as in normal play).
  The detour forwards every call to the original. Only while an export
  holds the lock, right before forwarding, it writes [previous counter] =
  QPC() - ticks(step), so the original's own subtraction yields the
  export step (plus the few ticks between our QPC read and its own). The
  delta the engine really used is read back after the call from the
  system time set (its +0x0C, warp 1.0, never clamped) and the
  difference is carried into the next frame (StepPlanner): no drift. The
  original overwrites [previous counter] with a real QPC at the end of every
  call, so nothing fake survives the lock.
Signature checks: the prologue, the two instruction sites that read and
store [previous counter] and the time set helper's store of +0x0C. Any mismatch
refuses the install (and the export is refused, never recorded on the
wall clock).
```

## The clamp of the clamped time sets

The clamp of the clamped sets ([min step], [max step]) is
read at install and logged: a step below the minimum (high fps times many
motion blur samples) would make the world run faster than the export;
the probe compares GET_FRAME_TIME with the planned step every tick and
logs the mismatches (RENDER_EXPORT,phase=timer_check).
