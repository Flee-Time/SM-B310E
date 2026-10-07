# games/ — game resource files for the B310E boot menu

This folder holds the **game data files** (WADs, ROMs, GRPs). The game
**programs** (.bin launchers) live in `fpbin/` — this folder only carries the
content the programs load. The menu scans emulator ROM folders automatically;
ported games remain explicitly configured in `fpbin/config.json`. An entry
requires its program binary; manual port resources are checked by that game.

## How to use

Copy the files from `F:\games\<game>` into the matching subfolder below, then
put the whole `sdcard/` folder onto a FAT32 card (the card root must contain
`fpbin/`, `progs/`, and `games/` side by side).

Filenames are **case-insensitive** (the FAT reader masks case) — `DOOM.WAD`
and `doom.wad` are the same file. Subfolders must keep their exact names.

## What goes where

| Card folder | Files the menu loads | Source (`F:\games\`) |
|---|---|---|
| `games/doom1` | `doom.wad` | `doom1/DOOM.WAD` |
| `games/doom2` | `doom2.wad` + `sandy.wad` (Into Sandys Cities) | `doom2/DOOM2.WAD`, `doom2/sandy.wad` |
| `games/duke3d` | `DUKE3D.GRP` | `duke3d/DUKE3D.GRP` |
| `games/sw` | `SW.GRP` | `sw/SW.GRP` |
| `games/heretic` | `HERETIC1.WAD` | `heretic/HERETIC1.WAD` |
| `games/hexen` | `HEXEN.WAD` | `hexen/HEXEN.WAD` |
| `games/wolf3d` | Complete registered GoodTimes 1.4 `.WL6` data set | `wolf3d/` |
| `games/blood` | Blood **1.21** `BLOOD.RFF`, `SOUNDS.RFF`, `BLOOD.INI`, `TILESxxx.ART`, tables and other installed game data | `blood/` |
| `games/retris` | (none — self-contained) | — |
| `games/snes` | Any `.sfc` or `.smc` ROM | `snes/` (currently only `Super Metroid.sfc`) |
| `games/gameboy` | Any `.gb` or `.gbc` ROM | `gameboy/Pokemon Silver.gbc` |
| `games/nes` | Any `.nes` ROM | `nes/` (currently only `SMB.NES`) |

Game builds include stereo audio, automatic headphone routing and common volume
controls: **hold END + #** to raise volume, **hold END + \*** to lower it.
Emulator save controls remain **END + 0**. Normal exits also save battery RAM;
removing power can lose unsaved progress. See `docs/game-ports.md` in the source
repository for supported formats and remaining audio limitations.

Wolfenstein data editions must match the binary: `wolf3d.bin` is registered
GoodTimes 1.4, `wolf3d_apo.bin` registered Apogee 1.4, `wolf3d_sw.bin` shareware
1.4, and `wolf3d_v11.bin` shareware 1.1. Change the explicit launcher entry when
using another edition. RFF files alone are insufficient to run Blood.
Do not copy the CD installer's loose `BLOOD.PAL`; it overrides the palette in
`BLOOD.RFF` with installer colors.
