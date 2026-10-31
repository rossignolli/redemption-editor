# Vehicle pieces and wagon wheels

Notes behind `src/vehicle_pieces.h` (RDR2 build 1491.50, offline dump and
recorded clips): how a wagon's wheel leaves the vehicle and why a recorded
wheel child is played from the vehicle's model.

## A wheel that breaks off

A wagon's wheel does not come off as a fragment piece: the break deletes the
wheel's groups from the vehicle (FRAG_BREAK where=vehicle, piece=0) and the
engine hands out a wheel object of its own (e.g. model wagon02x_wheel_lr
0x06A01A3E), kept per wheel slot in the vehicle's draft data. The wheel
break (kVehicleBreakOff) reads the draft data at vehicle +0xEE8 and returns
[draft + slot*8 + 0xA38]; the maker (kWheelMaker; this = the draft data,
[draft] = the vehicle, [draft+8] = its model info) stores the object it
creates there, of the model hash at model info +0x5B0 for slots 0 and 1 and
+0x5B4 for the others (slot/2), and links it to the vehicle (the vehicle is
registered in the object's extension). That object draws with the vehicle's
colours: its own tint bits (+0xB8) read 0 while it shows the wagon's tint
(OBJECT_LOOK tinted=0); the vehicle tint selects colour entries of the
vehicle's shader (the tint setter writes [vehicle+0x28]+0x574/0x575, mapped
through the model's colour table), not an object palette row, so
SET_OBJECT_TINT_INDEX on a plain object of the wheel model cannot give it (a
plain object of the model draws bright yellow, without its vehicle).

So, as a fragment child is created with its frag parent, a wheel piece is
recorded as a VehiclePiece whose group is kWheelPieceFlag | wheel index (no
frag group is that high: 0x80 bytes of group bits), with its own world track
from the moment it leaves, and played as the object _BREAK_OFF_VEHICLE_WHEEL
returns from the vehicle puppet (the engine makes it with its parent link,
the puppet's tint).

## A wheel still on its wagon

A wagon's wheel objects hang on the vehicle at its wheel bones; a ped near
the wagon gets them recorded as attached objects (POSE_V2_VEHICLE_CHILD,
1171-Blackwater 62:192, 65:193, 99:194, 108:196 of 1:2). They are the
vehicle's own parts, made by its wheel maker from its model's wheel models
(model info +0x5B0 front, +0x5B4 rear) and drawn with its colours; the
vehicle puppet makes its own when the engine wants them (only near the
camera: a far puppet has none yet). As the recording keeps a vehicle's
wheels with the vehicle, such a child is decided from the vehicle's
model, never from the live puppet: it has no object of its own while it is
attached to the recorded vehicle at the playhead, and the puppet keeps that
wheel (the part track's wheel bit is not applied: in 1171 the wheels went
"destroyed" at 66.7 s while their objects stayed on the wagon, drawn, to
the end of the recording). Once the record has left the vehicle (its
attachment ended), the part track and the loose wheel path apply as before.
The model info is [entity+0x20] (offline: the vehicle tint setter reads
it, then its colour count at +0x260).
