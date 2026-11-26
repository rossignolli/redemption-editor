# Optional cutscene audio

Cutscene voices are optional. Redemption Editor includes a map for 646 supported cutscene tracks and their timing,
but it does not include, download, or extract game audio. You must export audio from your own RDR2 installation.

## Extract and install the audio

1. Use an RDR2-compatible archive browser to find the game's `.awc` audio containers in its `.rpf` archives. For
   example, [OpenIV](https://openiv.com/) can browse RDR2 RPF archives and view AWC audio.
2. Export the relevant `.awc` containers from your own game files. Open each container in an AWC tool and export the
   tracks listed in [`cutscene-audio-tracks.csv`](cutscene-audio-tracks.csv) as WAV. One community option is
   [RDR2 Audio Tool](https://github.com/WesternSpace/RDR2AudioTool), which can export AWC streams to WAV. It is a
   separate, third-party tool and is not included with Redemption Editor; its project notes that it is no longer
   maintained. You can use another RDR2-compatible AWC exporter if you prefer.
3. Keep the container names and track hashes from the CSV. Place each WAV at the listed relative path under a folder
   named `CutsceneAudio`.
4. Copy that `CutsceneAudio` folder to:

```text
%LOCALAPPDATA%\RedemptionEditor\CutsceneAudio\
```

For example, the CSV may list an output path such as
`RMUD3_S1_INT_MASTERED_ONLY/0x08EC832B.wav`. The editor recognizes these layouts:

```text
CutsceneAudio\<container>\0x<TRACK_HASH>.wav
CutsceneAudio\<scene>\main.wav
CutsceneAudio\<mission>\<scene>\main.wav
```

For example, the RMUD31 replay can use either:

```text
%LOCALAPPDATA%\RedemptionEditor\CutsceneAudio\RMUD3_S1_INT_MASTERED_ONLY\0x08EC832B.wav
%LOCALAPPDATA%\RedemptionEditor\CutsceneAudio\RMUD3_S1_INT\main.wav
```

The editor matches the recorded scene to its built-in mapping, so you do not need to edit an INI file or configure
scene names. Keep the directory structure and filenames intact. Only WAV files are read by the mod. The game archives
are never extracted by the editor. Do not redistribute extracted game audio; the release contains metadata only.
