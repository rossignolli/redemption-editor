# Reporting a problem

Redemption Editor can pack everything we need to find a problem into one ZIP file. This page says how to make it and
what goes in it.

## After a crash

When the game crashes while the editor is running, the editor keeps a crash log and a crash dump. The next time you
start the game, a notice says **"Redemption Editor crashed last time"** (once per crash). Then:

1. Press **Shift+F1** (the Report key; it can be changed in `RedemptionEditor.ini`, section `[Keys]`, key `Report`).
2. Wait a few seconds: a notice says where the ZIP was saved. It goes in the `ReplayReports` folder of your game folder,
   for example `C:\Program Files (x86)\Steam\steamapps\common\Red Dead Redemption 2\ReplayReports\report-20261025-201500.zip`.
3. Open a **Crash report** issue on the project's GitHub page and attach the ZIP. GitHub takes files up to 25 MB: if
   the ZIP is bigger (crash dumps and clips add up), share it from a cloud drive and paste the link.

The notice only appears for a crash of the last 7 days (the ZIP carries the crash files of the last 7 days).

## When something looks or sounds wrong

1. While the replay plays, press **Shift+F2** at the moment the problem shows (a mark). Type a few words about what is
   wrong and press Enter. You can make several marks.
2. Press **Shift+F1** to save the report ZIP.
3. Open a **Bug report** issue on the project's GitHub page and attach the ZIP (a link if it is over 25 MB).

## What the ZIP holds

- `RedemptionEditor.log`, `RedemptionEditor.crash.log` (when there was a crash), `ScriptHookRDR2.log`, `ReShade.log` and
  your `RedemptionEditor.ini`.
- `report.txt`: the editor's version, the time, what the editor was doing and which clips are in the ZIP.
- `sysinfo.txt`: Windows, CPU, memory, graphics cards and drivers, the game's version, ScriptHook, ReShade and the
  other mods found in the game folder.
- `crashes\`: the crash dumps of the last 7 days, from `%LOCALAPPDATA%\RedemptionEditor\crashes`. A dump holds part of
  the game's memory at the moment of the crash.
- `marks\`: your marks (the note you typed and the screenshots of that moment).
- `clips\`: **only the clips you marked** with Shift+F2. A clip is a recording of the scene (people, horses, positions,
  sounds).

The files can show your Windows user name in folder paths. Nothing is sent anywhere: the ZIP stays on your disk until
you attach it yourself.

### Adding the open clip

By default the ZIP carries only the clips you marked. To also add the clip that is open in the editor when you press
Shift+F1, set this in `RedemptionEditor.ini` (in the game folder) and restart the game:

```ini
[RedemptionEditor]
ReportIncludeClips=1
```

## More detail in the log

The log is complete by default (`FullLog=1` in `RedemptionEditor.ini`). If you set it to `0` to keep the file small,
set it back to `1` before you report a problem, restart the game, make the problem happen again and save a new report.

## Advanced switches

Advanced switches exist for support; we'll tell you if you need one. They are not in `RedemptionEditor.ini` and they
are all on. If an issue on GitHub asks you to add one, put the line it gives you (for example `DecalGate=0`) under
`[RedemptionEditor]`, restart the game and save a new report. Remove the line again when the issue says so.
