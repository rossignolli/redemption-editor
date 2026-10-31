# Vehicle and train car doors

Notes behind `src/vehicle_doors_engine.h`: where the engine keeps a vehicle's
door ratios and the train car door latch (RDR2 build 1491.50, offline
analysis). The reads are rung 2 (layout proven below, signatures checked once,
SEH).

- IS_VEHICLE_DOOR_FULLY_OPEN's core maps the script door index
  through a table of hierarchy ids (a lea into .data) and finds the
  door with GetDoorFromId: the vehicle's doors are an array at [veh+0xBD0],
  count s32 [veh+0xBD8], 0x70 bytes each, hierarchy id s16 at +0xC; the
  door is "fully open" when the float at +0x28 is above 0.99 (flags
  +0x14, target +0x20, old target +0x24, current +0x28;
  SetTargetDoorOpenRatio stores +0x24, +0x20 and tests bit 18
  of +0x14, IS_ARTICULATED).
- SET_VEHICLE_DOOR_CONTROL takes vehicle types 0..11 and the
  trains (17..19) and sets the target with flags 0x4200; in this build the
  setter then steps the current ratio toward it (no instant set), so the
  puppet's door can lag: the readback decides.
- _0x762FDC4C19E5A981 (its handler: train types 17 and 18 only) ends in
  the latch setter, which stores its bool at [car+0x1680]; the
  decompiled train holdup scripts call it on the boxcar and refrigerator
  cars with the door scenes. Recorded and played as the latch channel.
