# Changelog

## 1.2.20 — November 26, 2026

### Lighting editor

- Added named lights with individual position, range, intensity, color, and temperature controls.
- Added saved multi-light rigs, including placement in front of the camera, group movement and rotation, individual
  light editing, and ungrouping. `Ctrl+Z` restores the previous grouped state after ungrouping.
- Added light preview controls, including solo preview, map selection and dragging, and fine movement while holding
  `Ctrl`.
- Added color presets and editor appearance themes: Blueprint, Frontier Classic, Crimson Red, Violet Outlaw,
  Midnight Frontier, and Original.
- Added the lantern emblem to the marker menu's Lighting entry and the lighting editor headers.
- Fixed rig insertion and selection behavior, preview control placement, and light-list/menu layout.

### Camera and timeline controls

- Made the timeline directly clickable for seeking and restored comma/period frame stepping.
- Improved lighting-menu/free-camera handoff: `F11` enters free camera with the menu hidden, and `F2` restores the
  menu at the current camera position.
- Added consistent `Escape`/`Backspace` navigation through nested lighting menus.
- Restored the camera blend choices, including spline, linear, smooth, and no blend, plus camera-follow behavior.

### Cutscenes and audio

- Fixed the one-frame pose/position pop immediately before cutscene camera cuts. The last valid pose now holds through
  the final pre-cut sample, and the next pose appears at the original cut time; normal interpolation continues elsewhere.
- Added support for cutscene voice playback from local WAV files, with built-in scene/track matching and cutscene end
  and No Control timing boundaries.
- Added a track list and installation guide for local cutscene audio. The release contains metadata only; users must
  extract WAVs from their own game installation.

### Release packages

- Added separate ReShade and No-ReShade editions. The No-ReShade edition includes the built-in Vulkan overlay; the
  ReShade edition uses the ReShade add-on.
- Added checksums and bundled third-party notices/licenses to both packages.
