# FLASH.md — hardware boot test (RAM load)

This is the procedure for loading B310E-OS into the phone's RAM over USB and
watching it run. It is written for the person holding the phone, and the
expected outputs below are what you should see if everything works.

## What this does, and does not

This procedure **RAM-loads** the OS. It writes nothing to the phone's NOR
flash, so the stock Samsung firmware is fully intact after any normal reboot.
There is zero brick risk: the worst outcome is that the phone does nothing,
and a reboot brings the stock OS right back.

## Files you need

- From the repo's `tools/spd_dump/` folder:
  - `spd_dump.exe` — the host loader.
  - `nor_fdl1.bin` — the FDL1 downloader spd_dump sends to the phone.
- `os.bin` — build it first (see `BUILD.md`); it lands in `build/bin/os.bin`.
- `libc_server.exe` — from the repo's `tools/libc_server/` folder. It is the
  host-side console for the USB debug channel.

> These tools are **not our code**: `spd_dump`/`nor_fdl1` come from
> [ilyakurdyukov/spreadtrum_flash](https://github.com/ilyakurdyukov/spreadtrum_flash)
> and `libc_server` from
> [ilyakurdyukov/fpdoom](https://github.com/ilyakurdyukov/fpdoom) — built
> from those upstream sources, distributed here for use with this project.

## Step 1 — verify the phone is in download mode

In a PowerShell window, from the `tools/spd_dump/` folder, run:

```powershell
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000
```

It waits for the phone. Now:

1. Remove the battery.
2. Hold down the D-pad CENTER key.
3. Plug in the USB cable while still holding CENTER.
4. Keep holding for a few seconds.

Expected console output:

```
Custom FDL1: CHIP ID = 0x6530c000
```

That CHIP ID confirms the phone is a real SC6530C. If CENTER does not work,
community-reported alternatives are the key "1" and the volume keys.

> **Replica warning**: recently bought SM-B310E units may be clones on a
> different chip. Verify the CHIP ID above before anything else, and do not
> proceed if it is not `0x6530c000`.

## Step 2 — load our OS

Still in the `tools/spd_dump/` folder, run:

```powershell
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 fdl ..\..\build\bin\os.bin ram
```

This loads `os.bin` at `0x34000000` and jumps to it. When spd_dump exits, the
phone is running our kernel.

## Step 3 — watch the console

Start `libc_server.exe` from the repo's `tools/libc_server/` folder. It opens the
phone's USB debug channel (VID:PID `1782:4d00`) and prints each kernel log
line prefixed with `!!!`.

Expected output, in order:

```
!!! connected
!!! banner: up
!!! keypad: up
!!! banner: tick 32
!!! banner: tick 64
...
```

The `banner: tick` lines repeat, one every ~32 scheduler yields.

## Step 4 — check the screen

The LCD should show:

- a blue background,
- the banner text with the firmware version near the top,
- `kernel: alive` below it,
- a blinking pixel in the bottom-right corner.

## Step 5 — keypad echo

Press keys. Each press should print on the host console and draw the key's
name on the LCD:

```
!!! key: <NAME>
```

The keymap is the **real extracted B310E matrix** (pulled from the stock
firmware dump), so all 20 keys — digits `0`–`9`, `*`, `#`, `DIAL`, `END`,
`LSOFT`, `RSOFT`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `CENTER`, `VOLUP`,
`VOLDOWN`, `MUSIC_PLAY`, `MUSIC_NEXT`, `MUSIC_PREV` — should echo. The END
key (EIC power button) works as a down+up pair.

## Step 6 — optional: flash the SD-card boot loader (sdboot3.bin)

Everything above is RAM-only. The **one flash-write step** in this project is
installing the SD-card boot loader so the phone can boot the boot menu from an
SD card with no USB cable. It is **reversible** — the same commands restore
the stock boot.

The loader is the stock fpdoom `sdboot3.bin` (3268 B, from the fpdoom release
`prebuilt_fix14` pack — we do not build a custom one). It sits at
`tools/spd_dump/sdboot3.bin`. After install: power-on with **any key held** →
sdboot reads `fpbin/fpmain.bin` from the card and runs the boot menu; **no
key** → normal stock boot, untouched.

From the `tools/spd_dump/` folder:

```powershell
# INSTALL the sdboot3.bin boot loader (writes NOR!)
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 `
  write_word fw+0x20 0x3fc000 `
  erase_flash fw+0x3fc000 0x1000 `
  write_data fw+0x3fc000 0 0 sdboot3.bin `
  write_word fw+0x3fc004 0x46e4

# REMOVE (restores stock boot)
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 `
  write_word fw+0x20 0x46e4 `
  erase_flash fw+0x3fc000 0x1000
```

Notes:

- This writes the NOR boot vector (`fw+0x20`) to point at the loader window
  (`0x3fc000`) and stores the original stock entry (`0x46e4`) at the loader's
  header `+4` so the no-key path chains back to stock.
- The erase covers one 4 KB sector (the loader fits in it).
- After flashing, stage an SD card (`make sdcard` → copy `sdcard/` onto a
  FAT32 card, add your game files per `sdcard/games/README.md`) and power on
  holding any key.
- The full boot-chain details live in [`docs/sdboot.md`](docs/sdboot.md).

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| No `CHIP ID` reply | Boot key wrong, clone unit, or USB driver missing | Retry the other keys (CENTER, "1", volume). Install a libusb/WinUSB driver for `1782:4d00` (e.g. via Zadig) if the device does not enumerate. Confirm the CHIP ID before continuing |
| Load succeeds but screen stays black | LCD init timing or power bits | The `make debug` diag images (os-diag-lcd/rot/sd) isolate LCD-vs-boot issues; report the state |
| Keys are not echoed | — | All keys are hardware-verified on the real keymap; re-check the USB link |
| Nothing on libc_server, but the screen works | Wrong libc_server build | Make sure you are running the SC6530 build from the fpdoom release, not another target |

## Safety and recovery

Steps 1–5 are RAM-only — nothing writes to flash, and a normal reboot
(battery back in, power button) restores the stock phone OS completely.

Step 6 (the sdboot3.bin install) is the single flash-write step. It is
reversible with the REMOVE command above, and the erase is confined to the
loader's 4 KB sector at `0x3fc000`. If you ever need to restore the whole
NOR, the stock `dump_firmware.bin` (16 MB, gitignored, never committed) is
kept in the repo as the restore reference and the source for all the firmware
analysis documented in `docs/`.
