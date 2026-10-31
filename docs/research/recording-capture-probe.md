# Capture probe: fragment and lamp breaks

Notes from the capture side read probes (`src/capture_probe.cpp`, the lights
sweep) about how a lamp or other fragment break could be observed directly.
RDR2 build 1491.50, offline dump. None of these ports is installed.

## Candidate ports for the break itself

```text
  0x6E38AC (delegate registered at 0x6D973F, no direct
  caller) creates the object of a broken fragment and sets
  bit 0 of [new+0xB0] (the bit HAS_OBJECT_BEEN_BROKEN
  tests) on the new object and its parent; whether it runs
  on every break is not proved.
  0xE482D4 (called from the impulse break 0x6F5B98, slot
  71 of the fragment instance vtable 0x33CBDB8) plays the
  break effects and, through 0x12A0B20 -> 0x633CB4, sets
  bit 13 of [light+0xB0] for each child light: the light
  goes out by state query in its update, not by an event.
  Not every kind of break goes through it (the parts
  broken off override 0x6EAE68, slot 87, uses 0xE47B50).
```

## Why the light intensity does not show the break

The per-object intensity _SET_LIGHT_INTENSITY_FOR_OBJECT
writes (object+0x734, 0x10CF2A2) is not touched by the
break path: _GET_OBJECT_LIGHT_INTENSITY (0x10C6184) reads
the model's light attribute, not that field, which is why
a broken lamp never reads as "dark" and only its handle
vanishing (counted above) shows the break.
