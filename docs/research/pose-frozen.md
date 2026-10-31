# Frozen poses off camera

Background and measurements behind the frozen pose check. The code keeps the contract and a link here.

## Frozen poses off camera (src/frozen_pose_check.h)

The engine stops animating a ped every frame when it is off the game
camera; the recording has to keep it animated, and in 1.0 it did not: the
share of frozen pairs off camera went from 0 to 0.4% to 7 to 18%, with the
peds in the camera's view unchanged. No test saw it (the core tests run no
game, the benches ran the developer ini).

A carried body does not count: a wagon driver of 1051-Valentine repeats 100% of his pairs, on camera as well.

