# Authored light direction

## Finding

The bundled RDR2 native database exposes `GRAPHICS::DRAW_LIGHT_WITH_RANGE` as a per-frame light call with eight inputs: position, RGB color, range, and intensity. It has no direction vector, cone angle, or attenuation curve. The other light-related natives in the bundled database set properties on lights already owned by entities; none creates a free-standing spotlight with editable direction.

## Current editor behavior

The point-light path calls `DRAW_LIGHT_WITH_RANGE` once. The optional beam setting calls it five times along an axis computed from the saved aim pitch and yaw. Beam angle controls the sample radii. Each sample is still omnidirectional, so this creates a directional-looking cluster and can spill outside the intended cone. Range and intensity are the only native falloff controls available to this editor.

Aim, spread, position, and intensity are stored in the authored-light track and evaluated from the replay playhead on each script tick. The beam calculation contains no frame-to-frame random state, so scrubbing and exporting use the same direction and sample positions for a given light state.

## Scope

The current native interface does not provide a true dynamic spotlight. A true implementation would need a validated renderer path or an engine light insertion hook that carries direction, cone, and attenuation through the scene lighting pass. The project has no such path today. Keep the existing point-light mode and label the stacked version as an approximation until one is proven.
