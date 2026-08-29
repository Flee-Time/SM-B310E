# games/ — game resource files for the B310E boot menu

This folder holds the **game data files** (WADs, ROMs, GRPs). The game
**programs** (.bin launchers) live in `fpbin/` — this folder only carries the
content the programs load. The boot menu shows each game only when its files
are present, so a game whose files are missing simply doesn't appear.

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
| `games/retris` | (none — self-contained) | — |
| `games/snes` | `Super Metroid.sfc`, `F-Zero.sfc`, `EarthBound.sfc`, `Super Mario All-Stars.sfc`, `A Link to the Past.sfc` | `snes/` (currently only `Super Metroid.sfc`) |
| `games/gameboy` | `Pokemon Silver.gbc` | `gameboy/Pokemon Silver.gbc` |
| `games/nes` | `SMB.NES`, `Super Mario Bros. 3.nes`, `The Legend of Zelda.nes` | `nes/` (currently only `SMB.NES`) |
