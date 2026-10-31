# Camera cuts in the replay

Notes behind `src/camera_cut.h`: why the replay places its cameras with
SET_CAM_PARAMS and how the cut rate is measured (RDR2 build 1491.50, offline
dump).

## The cut bits

SET_CAM_COORD and SET_CAM_ROT always set the cut bits 0x40 (position) and
0x80 (orientation) of the camera frame's flags [cam+0xEC] (their native
handlers' cores). The frame is copied to the final rendered frame
(kCameraFinalFrameFlags), whose "cut" test (kCameraCutTest) feeds the post
process: the TAA drops its history on a cut frame. Placed with those two
natives every tick, every frame would render as a cut (a jagged and
shimmering picture).

SET_CAM_PARAMS with duration 0 sets the same position, rotation and FOV
and raises the cut bits only when its p12 is true. Its rotation is applied
when [cam+0x538] == 3 and [cam+0x520] == 0: the base camera constructor
writes 3 and 0, only the POINT_CAM_AT_* paths write 1 or 2 and
STOP_CAM_POINTING writes 3 back; the replay's cameras never point. Its
position is applied when [cam+0x498] == 0 (not written by that constructor;
its role and default are not proven, hypothesis: an attach target). The
script still checks the first placements of each camera with GET_CAM_ROT /
GET_CAM_COORD and falls back to the plain natives when they did not take
(CAMERA_CUT_MODE,phase=fallback).

## Measuring the cut rate

The cut rate: the word at the final frame's flags, read once a tick (rung 2,
guarded) at the address the cut test's code gives (mov eax,0xC0; test
word [rip+disp],ax; seta al; ret), counted per second (CAMERA_CUT_RATE, kept
with Debug=0).
