# FNV Grass Stutter Fix

An xNVSE plugin for Fallout: New Vegas (1.4.0.525) that removes the hitches you get when turning or walking through dense grass. The grass stays the same. Only when and how often the game rebuilds it changes.

## Results

Measured with an automated in-game bench in a dense grass area (heavy grass settings, 900 frames of turning in place, then walking). The frame times are Present-to-Present.

| | Plugin measuring only | Plugin on |
|---|---|---|
| Turning: worst frame | 96 ms | **25 ms** |
| Turning: frames over 50 ms | 54 | **0** |
| Turning: average frame | 18.5 ms | 12.9 ms |
| Walking: worst frame | 104 ms | **38 ms** |

With the plugin on, turning in place costs the same as standing still.

## Why the game stutters

All of this is from the decompiled `FalloutNV.exe` 1.4.0.525:

- **Turning rebuilds grass.** `PlayerCharacter::UpdateCamera` calls `TESTerrainLODManager::UpdateGrass` (call at `0x94C130`) after a 64-unit move *or* a small turn of the view. `UpdateGrassForCell` (`0x57D4F0`) then deletes the grass of every land quad that is outside the view (`jz` at `0x57D93B` → `RemoveFromTerrainChunk`). Turning back creates it again, from scratch, on the main thread, in one frame. In the test area that was 55–78 ms per turn update.
- **Empty patches are retried forever.** A grass patch and grass type that produced no blades is never marked as done, so `CreateGrass` runs for it again on every update.
- **Everything new is built at once.** When a cell enters range, all of its grass is created in the same frame.

## What the plugin does

Each part has a switch in `GrassStutterFix.ini`:

| Setting | Default | Effect |
|---|---|---|
| `bKeepOffscreen` | 1 | Grass outside the view is kept instead of deleted, so turning never rebuilds it. It is still culled when drawing, so off-screen grass costs memory, not draw time. |
| `bSkipTurnOnly` | 1 | With `bKeepOffscreen`, an update caused only by turning has nothing left to do and is skipped. Moving still updates every 64 units as before. |
| `bMemoEmpty` | 1 | Remembers patch/grass-type pairs that came out empty, so they are not rolled again until that cell or chunk is unloaded or you jump more than one cell. |
| `iBudget` | 150 | During normal updates, at most this many grass patches farther than `fNearRadius` are built per frame. The rest follows in the next frames. 0 = no limit. |
| `fNearRadius` | 3000 | Patches closer than this (game units) are always built at once. |
| `bLog` / `fSlowMs` | 1 / 8 | Writes `logs\GrassStutterFix.log` in the game folder: every grass update slower than `fSlowMs`, and a summary every 10 seconds. |

Loading a save, fast travel, teleports and the `tg` console command still do a full grass rebuild as before.

## Things to know

- Grass behind you stays loaded, so memory use is a little higher.
- With `bMemoEmpty`, a very sparse grass type gets one roll per patch instead of being rerolled until something grows. Very thin grass can look slightly thinner.
- After crossing a cell, the farthest grass can take a few frames to fill in. Raise `iBudget` if you notice it; lower it if walking still hitches.

## Compatibility

- **Requires:** xNVSE and the 1.4.0.525 game executable.
- **Patch safety:** every patched spot is checked first. If another plugin already changed one, that part is skipped and the log says so.
- **Other mods:** works with New Vegas Reloaded / Vegas Shaders, JIP LN and grass replacers. Grass mods only change how the grass looks, not this code.

## Install

Install the release zip with your mod manager. Manually, copy `GrassStutterFix.dll` and `GrassStutterFix.ini` to `Data\NVSE\Plugins`.

## Building

Visual Studio 2019 (v142) or newer, Win32 Release:

```
MSBuild GrassStutterFix.vcxproj -property:Configuration=Release -property:Platform=Win32
```

The output is `build\GrassStutterFix.dll`. The `nvse` folder holds the two xNVSE headers the plugin needs.

## License

GPL-3.0, see `LICENSE`.
