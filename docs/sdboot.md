# SD-card boot — B310E-OS (fpdoom sdboot as the NOR loader)

This documents how the B310E boots **from an SD card** — no USB cable, no PC:
the NOR loader checks the keypad, reads `fpbin/fpmain.bin` from a FAT32 card
and runs it.

**The loader is the stock fpdoom `sdboot3.bin`** (`tools/spd_dump/sdboot3.bin`,
3268 B, CHIP=3 — taken directly from the fpdoom release `prebuilt_fix14` pack;
we do NOT build a custom sdboot anymore).
It is a complete proven loader: keypad check → full chip init → SD read of
`fpbin/fpmain.bin` → copy to PSRAM → jump. The payload is the **fpdoom fpmenu
app built as `fpmain.bin`** (the official fpdoom release layout — NOT our
`menu.bin`, see below).

## Boot behavior

| Power-on | What happens |
|---|---|
| **any key held** | sdboot (`SDBOOTKEY=-1` in the header, `sdbootkey` = 0xffffffff) → chip init → reads `fpbin/fpmain.bin` from the card → copies to PSRAM `0x04000000` (NOR-boot window, MEM_REMAP=0) → jumps ARM-state → the fpmenu app runs its game menu |
| **no key** | sdboot chains to the original stock entry (`0x46e4`, stored at its header +4 by the install) — **normal stock boot, untouched** |

## What fpmain.bin is (and why our menu.bin is NOT it anymore)

The official fpdoom release builds `fpbin/fpmain.bin` from the **fpmenu** app
(`release.make`: `%/fpmain.bin: makebin fpmenu OPTS NAME=fpmain`). The app
format (verified against the stock build):

- **`fpmain.bin` (stock fpmenu) = 29684 B** = part2 stub (512 B) + part1 app
  (29032 B, linked at `0x14000000`, self-relocating via its appended reloc
  table) + relocs.
- sdboot loads the whole file to `0x04000000` (NOR boot) and jumps there.
  part2's `_start` (`bl __image_end`) branches into part1's `start.s`, which
  relocates itself for the `0x04000000` runtime base, then `entry_main` →
  `sys_init` (LCD/keypad from the firmware-dump scan) → `main()`.

**The port is DONE (2026-08-23): the current `fpmain.bin` (30252 B, built by
`tools/fpmain/build-fpmain.ps1`) is the fpmenu skeleton with OUR boot menu**
— **BOOT STOCK** + the card's **progs/** (OS-like / single apps, no args) +
the **fpbin/config.txt games** (the PORTS section, launched with their
`--dir`/game args via `readconf.h`).

**LAUNCH = the stock fpdoom readbin mechanism** (OPTION B, learnings #32):
part2's readbin is copied to IRAM `0x40004000`, then it reads the selected
file **directly to `0x14000000`** (the fpdoom app window) and jumps. No
MEM_REMAP, no SMC re-init, no LCDC handling — the file overwrites the
framework's own PSRAM (0x14000000 aliases the same physical memory), and the
readbin survives from IRAM. (The stock's `__image_start & 0xfc000000` load
target is SC6531E-only — it computes 0 on the SC6530 NOR boot; our menu uses
the explicit `0x14000000`.)

**os.bin is now RELOCATABLE** (the fpdoom mechanism, `make os-sd`): linked at
`0x14000000`, built `-pie`, a pack_reloc `.rel` table appended, and
`arch/start.s` applies it (diff = runtime − link). It runs at `0x14000000`
via the readbin (diff 0), or self-relocates to `0x34000000` when USB-loaded
(`spd_dump fdl os.bin ram` — the SC6530 ram_addr, spd_dump.c:1319). The old
remap-cascade stub (`menu_stub.s`) is retired (gc'd out). The stock fpmenu is
kept as `tools/fpmain/fpmain.bin.stock-backup`.

The old `make fpmain` target (our `menu.bin` built as the payload) is
**obsolete**: it assumed sdboot loads the payload to `0x34000000`, but the
real loader copies to `0x04000000` (MEM_REMAP=0), where our menu cannot run.
Both `make menu`/`menu.bin` (the merged-boot menu) and `make fpmain` were
**removed from the Makefile 2026-08-28** — the boot menu is exclusively the
ported fpmain from `tools/fpmain/` (below).

## History — why "our menu.bin as fpmain.bin" never loaded

The sdboot3.bin that was previously NOR-installed (2344 B, from the 2026-08-23
bundled-menu experiment) was built from the **B310E-modified source**, whose
SD-mode `sdmain` does:

```c
if (!embedded || *(uint32_t*)embedded != MENU_MAGIC /* "BMEN" */)
    return 1;   /* no bundled menu → chain to stock boot */
```

It **never reads the SD card** — it only runs a menu physically appended to
the loader image in NOR (none was appended → `0x3fc928` = erased flash =
0xffffffff ≠ "BMEN" → stock boot). The binary proved it: contains the `BMEN`
magic at offset 1920, contains **no** `"fpmain"` string (the card path was
compiled out by `gc-sections`). So `fpbin/fpmain.bin` was ignored entirely.
The clean rebuild (this doc) restores the original card-reading `sdmain` and
ports in the B310E keypad fix (full row/col init + column-aware decode) so the
held key latches reliably — the original minimal `read_key` misses it on the
SC6530.

## SD card layout

```
fpbin/fpmain.bin   the sdboot payload = OUR boot menu (the ported fpmain,
                   built by tools/fpmain/build-fpmain.ps1)
fpbin/config.txt   the PORTS menu (fpdoom format: |Name| path args ...) —
                   the games listed here launch with their args
fpbin/*.bin        the fpdoom game binaries (fpdoom.bin, infones.bin, ...)
games/             the game resource dirs (doom1/, nes/, ...) the config
                   points into
progs/             OS-like / single apps — listed by the menu (e.g. os.bin)
```

`make sdcard` stages `sdcard/` for a card: `fpbin/fpmain.bin` (the PORTED
boot menu, 32968 B) + `progs/os.bin` (= `os-sd.bin`, the relocatable USB-free
OS) + `progs/rockbox.bin` + `.rockbox/` + the game binaries and empty
`games/` folders. Copy the whole `sdcard/` folder onto a FAT32 card root,
then copy your own game data files into `sdcard/games/` per the README it
generates there (items whose files are missing are skipped).

## Build (the ported fpmain menu + the card)

The fpdoom clone lives at `build\fpdoom` (cloned automatically by
`tools/fpmain/build-fpmain.ps1` on first use; clean, commit d87f762). The
loader itself is **not** built — we use the stock prebuilt `sdboot3.bin`
from the fpdoom release.

```powershell
# PATH needs: arm-none-eabi-gcc, gcc (WinLibs), make, and git bash for sh
$env:Path = "C:\Program Files\Git\usr\bin;" + $env:Path

# 1. the PORTED fpmain app (our boot menu on the fpmenu skeleton) — stages
#    sources into fpdoom\fpmenu, builds, copies to tools\fpmain\fpmain.bin +
#    sdcard\fpbin\fpmain.bin, restores the clone.
powershell -ExecutionPolicy Bypass -File tools\fpmain\build-fpmain.ps1

# 2. os.bin / os-sd.bin — RELOCATABLE (linked 0x14000000, -pie, .rel appended
#    by the vendored pack_reloc; start.s applies it). Run WITHOUT git bash on
#    PATH (the repo's check/objcopy recipes run under cmd):
make            # os.bin (USB build)
make os-sd      # os-sd.bin (SD build: -DSD_BOOT_NO_USB) -> sdcard/progs/os.bin
make sdcard     # stage the whole card (fpmain + games + os + rockbox)
```

Verified headers: sdboot3.bin `+0 b` (ARM), `+4 0xffffffff` (install writes
0x46e4), `+16 __image_size` = file size, `+20 sdbootkey` = 0xffffffff (any
key), contains `"fpbin/fpmain.bin"`, no `BMEN` magic. fpmain.bin offset 0 =
`bl __image_end` (ARM) → part1 at +0x200.

## NOR install (REVERSIBLE — the flash step)

```powershell
# from tools/spd_dump/  — the prebuilt loader is 3268 B, fits one 4 KB sector
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 `
  write_word fw+0x20 0x3fc000 `
  erase_flash fw+0x3fc000 0x1000 `
  write_data fw+0x3fc000 0 0 sdboot3.bin `
  write_word fw+0x3fc004 0x46e4

# remove (restores stock boot)
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 `
  write_word fw+0x20 0x46e4 `
  erase_flash fw+0x3fc000 0x1000
```

Behavior after install: power-on with **any key held** → sdboot → our boot
menu (the ported fpmain); **no key** → sdboot chains to `0x46e4` → **normal
stock boot untouched**.

## Verification status

- [x] Root cause of "menu.bin doesn't load": the installed sdboot3.bin was the
      modified **BMEN bundled-menu** build that never reads the SD card
- [x] **Stock prebuilt `sdboot3.bin`** (fpdoom release `prebuilt_fix14` pack,
      3268 B) — verified: reads `fpbin/fpmain.bin`, no BMEN, sdbootkey=-1
      (any key)
- [x] **USER HW TEST: sdboot3.bin + stock fpmain (fpmenu) BOOTS** —
      the sdboot → fpmain chain works from NOR (user-confirmed 2026-08-23)
- [x] **PORTED fpmain.bin (32968 B)**: fpmenu skeleton + our menu (BOOT
      STOCK + progs/ + config.json games, 5x7 font, frame, battery, END-
      reboot). Launch = the stock readbin to `0x14000000` (the old remap
      cascade menu_stub.s is retired/gc'd). `make check` PASS (entry
      `0x14000010`, `.text` `0x14000000`).
- [x] **os.bin RELOCATABLE (OPTION B)**: linked `0x14000000`, `-pie`, `.rel`
      appended (vendored tools/pack_reloc/), `arch/start.s` applies it —
      runs at 0x14000000 (SD readbin) or 0x34000000 (USB ram). os.bin
      20482 B, os-sd.bin 19774 B. menu.bin/diag link `link/menu.ld`
      (0x34000000, fixed).
- [x] **History of the launch hunt (learnings #24–#33)**: text-stride
      mangling → LCDC-refresh disable (DMA reads the moved window) →
      os-sd USB guard (unpowered block stalls the AHB) → I/D-cache
      coherence on the IRAM stub call → FIFO-gated backlight markers
      (the keypad-light block is never powered by the framework) →
      **replaced wholesale by the readbin mechanism** (no remap at all).
- [ ] **User HW test (the readbin launch — PENDING): copy `sdcard/` to the
      card, hold any key → menu (BOOT STOCK + os.bin + config games whose
      bins exist) → select os.bin → READING/STARTING → os.bin boots.** This
      is the first test of the SC6530 `0x14000000` alias + the relocatable
      os.bin. Add the fpdoom release's `fpbin/*.bin` + `games/` to test the
      PORTS section.

## Full-NOR backup / restore

Take a full 8 MB backup before ANY NOR write (the menu install above, or the
audio EQ/mic patches from `b310e-audio-eq-tune`). Grammar below verified against
the external `spreadtrum_flash` clone's `spd_dump.c` (also T8 of
`b310e-audio-eq-tune`, evidence `.omo/evidence/b310e-audio-eq-tune/task-8-*.txt`).

| Subcommand | Address grammar | spd_dump.c evidence |
|---|---|---|
| `read_flash` | **digit-start literal only** (`str_to_size`) - `fw+` NOT accepted | :1364 (read_flash handler :1359) |
| `erase_flash` | `fw+`-relative (`str_to_addr`) | :1411 |
| `write_data` | `fw+`-relative (`str_to_addr`) | :1396 |
| `write_word` | `fw+`-relative (`str_to_addr`) | :1379 |
| `read_data` | **does NOT exist** - no such subcommand anywhere | 0 hits |

`fw_addr = 0x30000000` (:1320) is the NOR flash XIP window on the SC6530 - a
digit-start literal `0x30000000+<off>` (read_flash form) and `fw+<off>`
(erase/write form) address the SAME flash byte.

**Full 8 MB backup — NOT POSSIBLE (FDL read broken):** the custom FDL's
`read_flash` returns the SAME fixed 4 KB boot block for every requested address
— a backup attempt yields one block repeated 2048× (diagnostic:
`.omo/evidence/b310e-audio-eq-tune/task-diagnose-0x684000-b310e-audio-eq-tune.txt`).
`full-backup.bin` is therefore INVALID and must NOT be used for any restore.

`0x800000` = 8 MB = the whole NOR. The only valid full-NOR reference is a
dump-derived image: `dump_firmware.bin`'s first 8 MB (offset == XIP; the 16 MB
dump is this NOR replicated twice), sliced to its own file.

**Full restore (dump-derived; reverts every byte of NOR):**

```powershell
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 `
  erase_flash fw+0x0 0x800000 `
  write_data fw+0x0 0 0 <dump-8MB-file>
```

`<dump-8MB-file>` = the `dump_firmware.bin` first-8-MB slice (offset == XIP).

**Bootloader disclosure:** a full dump restore reverts EVERYTHING to the dump's
snapshot — the custom **0x3FC000 menu loader** (the phone's real boot vector,
confirmed NOR-installed) AND any user data since the dump are replaced. Re-run
the install commands above to keep the loader, or accept the dump's boot state
(stock). To keep the custom loader while reverting only the audio patches, use
the targeted pristine-sector revert below instead of a full restore.

**Targeted patch / revert (sector-granular, the audio EQ/mic patches):**

The pristine sector files are already staged (`tools/spd_dump/<sector>.pristine.bin`,
dump-derived + byte-verified — NOT captured via `read_flash`, which is broken):

```powershell
# PATCH one affected 4 KB sector (e.g. 0x7F4000):
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 `
  erase_flash fw+0x7F4000 0x1000 `
  write_data fw+0x7F4000 0 0 mypatch.patched.bin
# REVERT one affected 4 KB sector (restore the pristine bytes):
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 `
  erase_flash fw+0x7F4000 0x1000 `
  write_data fw+0x7F4000 0 0 mypatch.pristine.bin
```

- **Keep the cable seated between `erase_flash` and `write_data`** - a drop there
  leaves the sector erased (0xFF); recovery = re-run the write (patched or
  pristine). **No `read_flash`/`fc` in this path** - reads are not possible.
- **`erase_flash` is MANDATORY before any `write_data`** - `sfc.c:104` implements
  Page-Program only, so NOR 0→1 bit transitions silently fail without an erase.
- Optional verify: `read_flash 0x30000000+<sector> 0 0x1000 check.bin`, then
  `fc /b` compare against `.patched.bin` (must report "no differences").
