# NOR boot loader — B310E boot (replaces fpdoom's sdboot3 and fpmain)

`boot/` is our own NOR boot loader and boot menu, GPLv2+, with no fpdoom
code. It sits where fpdoom's `sdboot3.bin` was (NOR 0x3fc000; the boot
vector word 0x20 already pointed there) and carries the menu itself. The
card needs no `fpbin/` folder; the fpmain launcher, its JSON config and
`ports/fpmain/` are gone from the repository. The fpdoom era is described
under [History](#history-fpdoom-sdboot3-and-fpmain-2026-08-to-2026-11).

**Status (2026-11-07):**

- v1.0 (12.4 KB, default stock) was flashed and works on the phone: stock
  boot without a key, the menu with a key, B310E OS and Rockbox from it,
  and B310E OS Power → Restart to menu (the reset-mode byte survives the
  watchdog reset on the hardware).
- v1.1 (26.9 KB, 7 sectors): the menu by default, the B310E OS look,
  `payloads/`, Stock firmware as a direct jump. On the phone the text and
  icons came out as solid boxes, and the stock jump ended with the phone
  switched off (so did v1.0's reset with the byte cleared).
- v1.2: the menu only after a power-on with END (the charger boots stock),
  battery and charger at the top right, autoboot, two-row cards for long
  names, a temporary **Start stock** list. On the phone it always started
  the stock firmware, even with a key held: its 0x30000000-window
  self-check failed (see "NOR past 4 MB").
- v1.3 (34 KB, 9 sectors): v1.2 with the flash size set to 8 MB as the
  stock PBL does, a linear layout again, a longer END check (debounce on,
  up to 300 ms) and the keypad light blinking 3 times if the upper NOR
  still reads wrong. On the phone the menu came up on an END power-on
  (on battery; with USB in, the phone is already on from the charger, in
  the stock firmware's off-charging mode, before END is pressed). The card
  read failed ("Card error"), the corners looked square, and every Start
  stock line ended in the stock firmware's off-charging state or switched
  off.
- v1.4 (34 KB): Stock firmware asks to hold END and jumps into the stock
  PBL while it is held; the card is opened before the first battery read
  (the ADC's first conversion runs an analog recovery block), and a card
  failure shows its code; corner radius 4. On the phone the stock firmware
  booted with END held, faster than a normal power-on, but with a dark LCD
  until its own sleep and wake; the card failed with "FAT error 12" and the
  corners were drawn as squares.
- v1.5 (34 KB): `.data`/`.bss` moved to IRAM 0x40000000..0x40004000 (where
  fpdoom's sdboot kept its `.bss`). At 0x4000c868 FatFs's volume table
  read back 0 after it was written (FR_NOT_ENABLED, error 12), and gfx.c's
  corner table up there drew squares: on the phone that IRAM does not hold
  the loader's data at this stage of boot; QEMU models 256 KB and showed
  nothing. Before the stock jump the display is left running (clocks on,
  the LCM in command mode): the stock firmware never initializes the panel
  or the LCD controllers at boot (QEMU's LCM trace of a cold stock boot:
  only id reads 0xda-0xdc and windows), and only its wake ungates the LCDC
  and LCM clocks that v1.4 had gated. On the phone the card, the payloads
  and autoboot worked; the stock firmware still came up with a dark LCD
  after the jump, and END was sometimes missed at power-on.
- v1.6 (34.5 KB): Stock firmware resets by the watchdog with END held
  (request 0xb0, chained at once) instead of jumping: the stock firmware
  does not set the panel up on the path a jump enters (QEMU, with the
  panel's id reads answered, shows its ST7735 init, NOR 0x7ca68, run at
  neither a cold boot nor after a jump; the user saw the first
  "SM-B310E Samsung" screen missing). The menu also comes up with no
  charger and no stock reset reason in the byte (on battery only END
  switches on); holding END 2 s in the menu powers off. On the phone the
  menu then came up at every battery insertion, and Stock firmware came
  back to the menu (a reset with END held is a power-on: the request was
  gone).
- v1.7 (34.6 KB): what the phone showed about the stock firmware's "off":
  power applied (a battery put in, a charger, END) always runs code, and
  the stock's off state is its own code waiting for END. So without END,
  a key, a charger, a request or a stock reset reason the loader switches
  the phone off, and the next END is a real power-on through the loader.
  Stock firmware: OK, request 0xb0 and a watchdog reset without END, the
  stock waits as after a battery is put in, and END starts it on its own
  path. On the phone that END came back to the menu: the stock's "wait"
  is a switch-off, and END a new power-on.
- v1.8 (34.6 KB): Stock firmware back to "Hold END" and the jump, now
  with the LCM's eight registers back at the values found at power-on and
  the LCDC reset (the stock reads the panel's id through the LCM before it
  sets the panel up; v1.5 left the loader's fast bus timing there). The
  SD card is checked first: no answer to CMD8 or CMD55 is "No card" at
  once, nothing is loaded. 22 QEMU cases pass; flashed 2026-11-08. On the
  phone the stock LCD stayed dark.
- v1.9 (34.8 KB): before the jump also the backlight and keypad light
  back to their power-on values, and the panel put to sleep and reset
  (its reset line low, then back at its power-on level), so the stock
  finds the display as after a real power-on. 22 QEMU cases pass (QEMU's
  panel has no reset line and never runs the stock's panel set-up, so its
  screen after the jump is black). Superseded by v1.10 before it reached
  the phone.
- v1.10 (34.9 KB): v1.9 plus IRAM banks 1..3 enabled in chip_init
  (APB 0x8b0001a0 |= 7 << 19; bits 15:0 and 16, the DSP boot address and
  ARM_BOOT_EN, left alone). They are off at power-on, which is why IRAM
  from about 0x4000c000 did not hold data (v1.4); fpdoom's menu set them at
  entry, so every payload it started had them. B310E OS and Rockbox bounce
  SD DMA through IRAM 0x4000b000..0x4000f000: without the banks reads of
  more than 8 blocks came back wrong (a Rockbox MP3 crashed; found by the
  OS drivers session). Before the stock jump the three bits go back to
  their power-on value. 22 QEMU cases pass; flashed 2026-11-08. On the
  phone the RAM banks fixed Rockbox's MP3 crash. The stock firmware still
  started with its display off after the jump (one END press lit it); with
  END let go at once it switched off (it needs END held for a while).
- v1.11 (34.8 KB): the power-on END check no longer sets or restarts the
  EIC debounce (EIC_CTRL2/3, EIC_TRIG): it only unmasks END and the charger
  and reads them for up to 300 ms, and the mask goes back to its
  power-on value before the stock jump (saved by the early code at IRAM
  0x40003ffc). The suspicion: restarting the debounce cleared what the
  stock reads as "switched on by the power key", so it booted as if
  restarting, with no logo and the display off. 22 QEMU cases pass;
  flashed 2026-11-08. On the phone END still opened the menu; after the
  jump the stock firmware lit the backlight, set the user's brightness,
  but the panel showed nothing (its own sleep and wake fixes it).
- v1.12 (34.6 KB): the jump leaves the display "warm", exactly as the menu
  runs it (panel on, the LCM in its DMA data mode with its timing, the
  LCDC set up, clocks on): the stock's normal boot only sends window
  commands and kicks the LCDC, so it needs a display already set up.
  Earlier versions had changed the LCM mode (v1.5), its registers (v1.8)
  or reset the panel (v1.9). Stock firmware jumps at once if END is
  already held with OK. 22 QEMU cases pass; flashed 2026-11-08. Still no
  picture on the phone.
- v1.13 (34.8 KB): before the jump the stock's reset-mode marker is
  cleared as at a cold power-on: analog 0x82001140, 0x82001144, 0x82001148
  and the APB byte to 0. The stock keeps 0x1f1f / 0x7ee7 there while it
  runs (NOR 0x1a5ac) and the analog die keeps them while the battery is
  in; a boot that finds them looks like Spreadtrum's quiet restart after a
  crash (no logo, faster, the display off until a key, the brightness
  restored), which is what the phone showed. The Stock firmware screen
  shows the three values the loader found at power-on. 22 QEMU cases
  pass; flashed 2026-11-08. On the phone the values were all zero and the
  display still dark.
- v1.14 (34.9 KB, diagnostic): the stock's LCD init (NOR 0x16388) calls
  the panel's Init (ops[0], 0x7cc46) only when NOR 0x1057e returns 0, that
  is when bit 2 of analog 0x82001320 is set (0x1a2a0); the same test also
  skips 0x14bbc and 0x27c90. 0x82001320 is the stock's reset status
  (boot_mode.c: bit 7 is the reset pin, "ext_rstn assert", cleared through
  0x8200128c bit 11). Its own wake (sleep op with 0) calls the same Init,
  which is why a sleep and wake brought the picture back. The Stock
  firmware screen now shows the register as read at power-on, before the
  loader touches the analog die, and live ("rst <power-on> <now>"); END
  must be held 1 s for the jump so the live value can be read with END
  down. 22 QEMU cases pass; flashed 2026-11-08. On the phone: 0x8044 at
  an END power-on, 0x8040 on the Stock firmware screen (0x8043 with USB
  in, 0x8047 at a power-on with USB in); END does not change it. Bits 0-1
  follow the charger; bit 2 is a power-on flag that is gone by the time
  the menu runs, and the display stayed dark.
- v1.15 (35.0 KB, diagnostic): the register is recorded along the
  start-up (before and after the END check, at boot_main, after
  chip_init, the pin table, the display, the card, the battery reading,
  the menu) and shown with the live value as two rows of five on the
  Stock firmware screen, to find the step that clears bit 2. 22 QEMU
  cases pass; flashed 2026-11-08. On the phone: 0x8044 up to and including
  the pin table, 0x8040 from the display step on (0x8047 / 0x8043 when
  charging).
- v1.16 (35.2 KB): the display step split up in the trace (lcd_start, the
  splash, backlight_on, keypad_open). The stock's own backlight code (NOR
  0x1a154 level, 0x1a186 on/off, 0x1a1ce and 0x1a218 set/clear pairs
  0x8800/0x4400 and 0x1100/0x2200 in 0x82001220) sits next to the bit 2
  reader, and our backlight_on() sets 0x1100 and never clears it. Before
  the jump the loader now switches the backlight off as the stock does
  (bits 5-6 cleared then bit 5; 0x2200 set then 0x1100 cleared) and waits
  up to 500 ms for bit 2. With bit 2 back it jumps; without it the light
  comes back on and the menu says "rst bit 2 stayed clear" (no jump). The
  emulator models bit 2 as "0x1100 off" (a guess, marked as such in
  sc6530_adi.c) so the QEMU jump case still runs. 22 QEMU cases pass;
  flashed 2026-11-08. On the phone: the Samsung logo appears and the stock
  firmware boots normally, reproducibly, as long as END stays held until
  the logo; END let go after the 1 s hold and the stock resets as at an
  ordinary short press (its own power-on check).
- v1.17 (34.9 KB): the diagnostics removed (no trace rows, no early
  captures); END down 100 ms starts the jump again; the backlight-off
  before the jump stays. Why END must stay held: the stock's start-up
  (NOR 0x169f2) sorts the power-on (0x169a6: END = GPIO 19, analog EIC
  channel 3, read through 0x35300; alarm; charger 0x31762; the "ADC
  UsbSwitchVoltage" factory check 0x22e3e) and, just before the logo,
  reads END again (0x3530a, 30 ms debounce). With END let go it carries on
  only with a charger (its charging screen), the factory USB-ID voltage,
  or byte 12 of NV item 22 (0x2c4c0) set; this phone's NV has 0 there
  (every copy of item 0x16 in the NV log), so it powers off (0x1a42e).
  The emulator does not show this (its held END latches).

## Boot flow

| At power-on or after a reset | What the loader does |
|---|---|
| END held at power-on, no request (default `menu`) | full init → the autoboot countdown, or the menu |
| no END, a charger in (its power-on), or a stock reset reason in the byte | chains to the stock PBL at once (its charging screen, its restart) |
| no END, no charger, nothing in the byte (a battery put in) | switched off: the next END is a real power-on |
| any keypad key held | full init → the menu |
| request 0xb1 (menu) | full init → the menu |
| request 0xb2 (B310E OS) | full init → `payloads/b310e-os.bin` (`progs/` on an old card; the menu if it fails) |
| request 0xb3 (named payload) | full init → the path in the PSRAM block (the menu if the block is missing) |
| request 0xb0 (stock once) | chains to the stock PBL at once: the steps sdboot took |
| default `stock` / `program:PATH` (build option) | the stock PBL at once / that payload, with no key and no request |

A keypad key held at power-on always gives the menu, over a request and
over the default. END is not in the keypad matrix: it is analog EIC channel
3, read in `boot_early()` for 40 ms after the ADI comes up.

The menu is drawn like B310E OS: its theme colours, status, title and
soft-key bars, list cards, Montserrat text and icons (`os/ui/gfx.c`,
`font_montserrat.c` and `icons.c` compiled in unchanged). It lists the
card's `payloads/*.bin` (B310E OS first, then by name, up to 24, with their
sizes; `progs/` when a card has no `payloads/`) and **Stock firmware**.
UP/DOWN (or 2/8) move; CENTER, DIAL or 5 start, after the key is let go;
LSOFT ("Auto") sets autoboot. A name too long for one line gets a card two
rows tall. A payload loads with a progress bar. A card in the last row says
what went wrong ("No card", "Not on the card", "Request lost", …). The
status bar shows the battery as B310E OS does: its level and percentage,
"chg" while charging, "full" when done (ADC channel 5, the charger on EIC
channel 2, its status on GPIO 8/9).

**Autoboot.** `payloads/autoboot.txt`:

```
entry = b310e-os.bin
timeout = 3
```

`entry` is a file in `payloads/`, or `stock`; `timeout` is in seconds (0:
at once). Without the file, or with no entry, the menu waits (autoboot off).
After a power-on with END the menu selects the entry and counts down in its
last row; any key stops the countdown. LSOFT on an entry makes it the
autoboot entry, and on the autoboot entry turns autoboot off; the loader
writes the file (FatFs with writing; `timeout` is kept). Requests and a
held keypad key skip autoboot.

**Stock firmware** shows "Hold END to start it"; once END has been down
for 100 ms the loader jumps into the stock PBL (0x46e4), and the user
keeps holding END until the Samsung logo. What the phone showed about
the stock firmware: power applied (a battery put in, a charger, END)
always runs code from NOR; the stock boots only with END held, else it
switches off (or shows its off-charging screen), and the next END is a
new power-on. So it must be entered while END is held: v1.0 to v1.3
(jumps and resets without END) ended off or charging; v1.6 (a reset with
END held) was itself a power-on and lost the request; v1.7 (a reset
without END, then END) came back to the menu. A jump with END held (v1.4,
v1.5) started it, faster than a power-on, but its LCD stayed dark until
its own sleep and wake: the stock reads the panel's id (0xda-0xdc, NOR
0x7c90a) through the LCM before it sets the panel up, and the LCM still
had the loader's bus timing. v1.8 puts the LCM's registers back to what
the loader found at power-on and resets the LCDC before the jump, with
the clocks left on; v1.9 also puts the backlight and keypad light back
and resets the panel. (The stock's own reset-mode setter, NOR 0x1a5ac,
writes its mode, the flash constant 0x1f1f, to analog 0x82001140 and
0x82001148 with the magic 0x7ee7 in 0x82001144, and to the APB byte: a
"running" marker for crash reporting.)

**Power off:** holding END for 2 s in the menu (after the power-on press
has been let go) switches the phone off as B310E OS does (the lights off,
LDO_PD_SET1 = 0x1f, then LDO_PD_SET0 = 0x3fff). Not on the "Hold END"
screen.

The jump enters the stock PBL (0x46e4) without a reset: the LCD controller, card
and keypad stopped and their clocks off, the clock and PSRAM controller
registers put back to the values they had at power-on (saved before the
loader's own init), every interrupt line masked, the PBL's warm-boot word
at 0x04000000 cleared, CP15 V=0, then the PBL runs its full init. In
August a jump into the PBL hung, but from a menu that ran in RAM with the
PSRAM window moved to 0x34000000; this loader runs from NOR and never moves
it. In QEMU stock reaches its "SAMSUNG mobile" screen this way (QEMU
does not model the stock's power-on decision). Request 0xb0 stays for
programs.

## Design

### Where it lives

**NOR past 4 MB.** At a power-on the flash controller (SFC, 0x20a00000)
decodes only 4 MB: reads from 0x400000 up come back wrong until its size
register 0x20a00200 holds 8 (MB). The stock PBL sets it early (NOR 0x10354,
called from 0x103c0 on the 0x46e4 path: 1, 2, 4, 8, 0x10 … by the flash's
size). v1.1 linked its font and icons above 0x400000 and drew them as solid
boxes; v1.2 ran that part through the 0x30000000 window, which did not
read the NOR at boot either, so its self-check fell back to the stock
firmware every time. v1.3 keeps `.early` (header, start-up, the boot
decision, the helpers it calls) below 0x400000 (asserted at link time).
Before anything above it runs, `start.S` writes 8 to 0x20a00200 and checks
a marker at the image's end. If the marker reads wrong, the keypad light
blinks three times and the stock firmware starts. QEMU's SFC model now has
the size register: at a stock-mode power-on NOR reads from 4 MB up return
all ones until it is 8.

| What | Where |
|---|---|
| Header, start-up, boot decision (`.early`) | NOR 0x3fc000, 0x438 bytes, below 0x400000 |
| The rest: code, fonts, icons | after it, past 0x400000 once the SFC size is 8 (34 KB in all = 9 sectors; `boot.ld` allows 64 KB) |
| Stack | IRAM below 0x40009000 (sdboot's stack, so the no-key path touches nothing sdboot did not) |
| `.data` / `.bss` (menu path only) | IRAM 0x40000000..0x40004000 (asserted); the status block first, at 0x40000200. IRAM banks 1..3 (from about 0x4000c000) are off at power-on; chip_init enables them for the payloads (v1.10) |
| SD bounce buffer | IRAM (`.bss`): SDIO DMA into PSRAM hangs the phone |
| Frame buffer | PSRAM 0x043f2000 (`lcd_back` for the OS's gfx.c), as in B310E OS |
| Payloads | read to PSRAM 0x04000000, up to 0x3f0000 bytes |

The stock dump has 0x3fc000..0x4a0000 erased (656 KB, inside the stock
code partition's padding). fpdoom's install used the first 4 KB of it.

### Header (NOR 0x3fc000)

| Offset | Field |
|---|---|
| +0x00 | `b reset` |
| +0x04 | stock PBL entry, 0x46e4 (sdboot kept it here too; built in, no extra `write_word`) |
| +0x08 | `"B310BOOT"` (B310E OS checks it: `arch_boot_loader()`) |
| +0x10 | image size |
| +0x14 | version (1.13) |
| +0x18 | default with no key and no request: 0 stock, 1 menu after an END power-on (built default), 2 program after an END power-on |
| +0x20 | default program, 48 bytes (`payloads/b310e-os.bin`) |
| +0x50 | CRC-32 of the image with this word zero (`mkboot.py` sets and checks it) |

### The no-key path

`start.S` and `boot_early()` (`hw.c`, all in `.early`) do what sdboot did
before chaining, then read END (the ADI brought up as sdboot's full init
did, the EIC clocks on, channel 3 unmasked, its debounce on and restarted,
up to 300 ms; v1.2's 40 ms with the debounce as reset left it is
suspected too short):
MMU and D-cache off, I-cache on, SVC mode with IRQ/FIQ masked, stack at
0x40009000, keypad clock and reset pulse, keypad enable, a 20 ms wait on the
system counter, keypad clock off, then `bx 0x46e4`. Two differences, both
reads: the reset-mode byte (below) is read, and cleared only when it holds
one of our values; a key press edge on any of the four slots counts (sdboot
looked at slot 0 only). `.data` and `.bss` are not touched on this path.

### The full path

1. Chip init, sdboot's values: 208 MHz CPU with the AHB at half (104 MHz;
   B310E OS `clock.c` expects this), the PSRAM controller (SMC), the ADI
   (analog die). Every interrupt line masked; the analog watchdog stopped.
2. The stock pin table, read at run time from this phone's NOR (0xc6ef0,
   149 pairs of pinmux 0x8c000xxx and analog 0x82001xxx writes): the fpdoom
   menu applied it at every boot, so B310E OS and Rockbox have only ever
   run after it. The table is checked whole first (addresses, the
   terminator); **0x8c0002a4 is never written**. No stock bytes are in the
   loader: it reads the table where it is.
3. UART clocks off (as the fpdoom menu did).
4. The ST7735S BOE panel and LCDC (B310E OS `lcd.c`), backlight 60 %.
5. SD card (B310E OS `sdio.c`, read only, 4-bit, 24 MHz) and FatFs R0.15a
   (read only, long names, FAT12/16/32).
6. The requested or default program, else the menu.

### Payload contract

A payload is a raw image linked at 0x04000000 (B310E OS, Rockbox). The
loader reads it whole to 0x04000000, checks the first word is an ARM
instruction (condition AL), waits for the selecting key's release, and jumps
to it in ARM state with: MMU and D-cache off, I-cache on (invalidated), CP15
V=1 (high vectors; Rockbox's crt0 maps 0xffff0000 because of it), SVC mode,
IRQ/FIQ masked, every INTC line masked, the keypad disabled with its edges
cleared, the panel showing "Starting". The fpdoom menu entered with the MMU
and D-cache on (control 0x0005327f); both B310E OS and Rockbox already
handle either. fpdoom apps linked at 0x14000000 with a relocation table are
not supported.

### Warm-restart handshake

The request travels in the **reset-mode byte, APB 0x8b000228 bits 15:8**
("HWRST1"). A watchdog reset keeps it; a power-on clears it. Evidence:

- The stock firmware uses this byte for the same job. The PBL writes 0x5a
  (NOR 0x2c00) and branches on 0x5a/0x6a/0x7a (0x3c5c). The main OS writes a
  reset mode there (0x1a5ac) and reads back 0x8a/0x9a after a reset
  (0x1a7f8, 0x1a840).
- On the phone, 2026-08-23: a stale 0x5a in this byte survived the watchdog
  reboot and made the next boot need END. Clearing it before the reset
  fixed BOOT STOCK (AGENTS.md, learnings #18).

| Value | Meaning |
|---|---|
| 0xb0 | the stock firmware, once |
| 0xb1 | the menu |
| 0xb2 | `payloads/b310e-os.bin` |
| 0xb3 | the payload named in the PSRAM block |
| anything else | not ours: left as it is for the stock firmware |

Our values avoid the stock's 0x5a 0x6a 0x7a 0x8a 0x9a. The loader clears the
byte when it takes a request, so a later crash or reset does not repeat it.
In QEMU the stock firmware leaves 0x1f there after booting; the loader
ignores it.

0xb3's block is at PSRAM 0x043fff00 (inside B310E OS's MMU table, which is
dead once it resets): `"B3RQ"` (0x51523342), a 56-byte NUL-terminated path,
then `~(magic + the sum of the 14 path words)`. The loader reads it after the
PSRAM controller init and clears the magic. Whether PSRAM keeps its contents
through the phone's watchdog reset is **not established**: the stock-spy
plan (docs/dsp-audio-route.md) relied on it, but no result of that run is
recorded. 0xb1 and 0xb2 do not depend on it.

B310E OS (`os/arch/sc6530c/power.c`): `arch_boot_loader()` checks the
header; `arch_boot_request(req, path)` writes the byte (and the block for
0xb3, cleaned from the D-cache); `arch_reboot()` then resets by the
watchdog. **Power → Restart to menu** uses 0xb1 (without the loader in NOR
it says so). **Restart B310E OS** chains `/payloads/b310e-os.bin` without a
reset (it keeps the clock), and falls back to 0xb2 when that fails. A watchdog reset clears the RTC; the OS
gets the time back from `clock.txt` and SNTP.

### QEMU model changes

- `sc6530_aux.c`: the reset keeps APB 0x8b000228 bits 15:8. Machine creation
  starts it at zero, as a power-on would.
- `sc6530_keypad.c`: enabling the scan latches a press edge for every key
  already held, as the phone does when it powers on with a key down. A
  reset after the first keeps the held keys (a finger stays on the key).

## Build and test

```powershell
.\build.ps1 boot                       # build/boot/b310e-boot.bin (default: menu)
$env:B310E_BOOT_DEFAULT = 'stock'; .\build.ps1 boot  # or program:payloads/b310e-os.bin
python boot/mkboot.py info build/boot/b310e-boot.bin
```

The QEMU test needs your own NOR dump; it stays local (`mkboot.py nor`
writes the emulator image under `build/boot/test/`, which is ignored):

```powershell
python boot/tests/test-boot.py      # --dump tools/spd_dump/full-backup.bin by default
```

Cases (v1.8, 2026-11-08, all 22 pass): a power-on with END gives the menu
(4 entries, one on two rows, panel 0x7c89f0, 148 pin writes); so does a
held key; the menu starts B310E OS; request 0xb1; Auto on B310E OS writes
`autoboot.txt`, the next power-on counts down and starts it, a key stops
the countdown, Auto again turns it off (the file checked in the card image
afterwards); Stock firmware: with END held the jump reaches "SAMSUNG
mobile" with no machine reset; requests 0xb2, 0xb3 (Rockbox from the PSRAM block), 0xb3 without a
block, 0xb0; a stock value (0x7a) is left alone; B310E OS **Power →
Restart to menu**; the `stock` and `program` defaults; a card with only
`progs/`; a power-on without END with a charger boots stock, without a
charger switches the phone off; END held 2 s in the menu powers off;
with no card the menu says "No card" after 0.7 s; an
`autoboot.txt` card starts Rockbox after 1 s; a corrupted marker at the
image's end (the upper NOR read wrong) boots stock.

## Hardware

Nothing below has been run. Each NOR write needs your explicit OK.
Commands run from `tools/spd_dump/`; the phone in download mode (hold
CENTER, plug in USB; see [flashing.md](flashing.md)). Download mode is the
boot ROM's, before NOR, so it works whatever the loader does.

### 1. Full NOR backup (first, before any write)

```powershell
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 read_flash 0x80000003 0 8M nor-backup-20261107.bin
python ..\..\boot\mkboot.py backup-check --backup nor-backup-20261107.bin `
  --reference full-backup.bin --slice nor-backup-20261107-8M.bin
```

Taken 2026-11-07 before the first install: `nor-backup-20261107-8M.bin`
(word 0x20 = 0x3fc000, fpdoom sdboot at 0x3fc000; a 16 MB read stopped at
15 MB when bootrom mode timed out, and the part of the second copy it got
matched the first). 8 MB is enough and quicker. `0x80000003` is the
whole-flash region `nor_fdl1.bin` knows (it maps it to
the NOR window plus the offset); fpdoom documents this read, and
`dump_firmware.bin` (16 MB, the 8 MB NOR twice) came from a whole-flash read.
The FDL copies a plain address as it is, ignoring the offset, which is how
the old attempt got one 4 KB block for every address (the
`0x30000000+offset` form in the history section). `backup-check` refuses that failure (few distinct
blocks), a read whose two halves differ below the file system, and a missing
vector table. It prints the boot vector word 0x20 (0x3fc000 with sdboot
installed) and what is at 0x3fc000 (expected: fpdoom sdboot, 3268 bytes).
Keep the 8 MB slice: it is the full restore image.

### 2. Install (word 0x20 is already 0x3fc000)

```powershell
copy ..\..\build\boot\b310e-boot.bin .
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 `
  erase_flash fw+0x3fc000 0x10000 `
  write_data fw+0x3fc000 0 0 b310e-boot.bin
```

`erase_flash` asks for a typed `yes`. The phone leaves bootrom mode after a
while and after each spd_dump run: unplug and replug it (in bootrom mode)
before every command. spd_dump prints nothing when a write succeeds; read
the area back to check:

```powershell
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 read_flash 0x80000003 0x3fc000 0x10000 readback.bin
```

Only if `backup-check` showed word 0x20 = 0x46e4 (no loader installed), add
`write_word fw+0x20 0x3fc000` as the last command, as the sdboot install did.
Keep the cable in between the erase and the write: an interrupted pair
leaves the sectors erased, and the PBL would jump into erased flash; re-run
the install (or the restore) from download mode.

Or check a full read (as the backup):

```powershell
python ..\..\boot\mkboot.py verify --flash nor-after.bin --loader b310e-boot.bin
```

### 3. Test on the phone

1. Power on with END, on battery: the menu (v1.8 and the battery in the status bar,
   real text and icons).
2. Plug the charger into the phone while it is off: the stock firmware
   (its charging screen), no menu.
3. Start B310E OS; then Rockbox. In B310E OS, Power → Restart to menu.
4. LSOFT (Auto) on an entry, power off and on with END: the countdown.
5. Stock firmware, hold END until the Samsung logo: the stock firmware
   runs with its LCD on.
6. Take the battery out and put it back: the phone stays off; END: the
   menu.

Done 2026-11-07 for v1.0 (default stock): stock without a key, the menu with
a key, B310E OS, Rockbox, and Restart to menu all worked.

### 4. Restore

Back to fpdoom's sdboot (the state before the install; its menu needs
`fpbin/fpmain.bin`, which this repository no longer builds):

```powershell
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 `
  erase_flash fw+0x3fc000 0x10000 `
  write_data fw+0x3fc000 0 0 sdboot3.bin `
  write_word fw+0x3fc004 0x46e4
```

Back to the plain stock boot (no loader at all):

```powershell
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 `
  write_word fw+0x20 0x46e4 `
  erase_flash fw+0x3fc000 0x10000
```

Everything, from the backup (also reverts any other NOR change since):

```powershell
.\spd_dump.exe fdl nor_fdl1.bin 0x40004000 `
  erase_flash fw+0x0 0x800000 `
  write_data fw+0x0 0 0 nor-backup-20261107-8M.bin
```

### SD card

```
payloads/b310e-os.bin   B310E OS (request 0xb2, the program default)
payloads/rockbox.bin    Rockbox
payloads/*.bin          any other raw image linked at 0x04000000
payloads/autoboot.txt   autoboot (optional; the loader writes it on LSOFT)
```

Rename an old card's `progs/` to `payloads/` (the loader still reads
`progs/` while there is no `payloads/`). `fpbin/` is not used any more: the
games are apps of B310E OS (os/README.md).

---

## History: fpdoom sdboot3 and fpmain (2026-08 to 2026-11)

Everything below describes the chain before `boot/`. It stays valid for a
phone that still has `sdboot3.bin` in NOR. Its "Full-NOR backup — NOT
POSSIBLE" note is superseded by the `0x80000003` backup above.

### SD-card boot — B310E-OS (fpdoom sdboot as the NOR loader)

> **2026-11-06:** the old test OS (`os.bin` / `os-sd.bin` from `firmware/`)
> was removed; the menu now launches B310E OS as `progs/b310e-os.bin`, a raw
> image linked at 0x04000000 like Rockbox (no relocation table). The
> `os.bin` build notes below are history.


### JSON menu and automatic ROM discovery (current)

`fpbin/config.json` is the only menu configuration. `config.txt` and its
variable parser are removed from our menu. The staged fpdoom startup code
also reads the JSON `system` array before display/keypad initialization.
Previously it exited when `config.txt` was missing, before our JSON menu
loader could run. A missing/empty config now leaves BOOT STOCK and progs/
available; malformed JSON briefly shows CONFIG ERROR, then opens that menu.

Ported games stay explicitly listed in `categories[].items`. Emulator ROMs
are discovered from `emulators[].directory` each boot. Defaults scan:

| Emulator | Folder | Extensions | Binary |
|---|---|---|---|
| SNES | `games/snes` | `.sfc`, `.smc` | `fpbin/snes9x_16bit.bin` |
| Game Boy | `games/gameboy` | `.gb`, `.gbc` | `fpbin/gnuboy.bin` |
| NES | `games/nes` | `.nes` | `fpbin/infones.bin` |

Extensions are case insensitive. Files retain their complete FAT filename
in launch arguments, including spaces; the narrow display truncates labels.
Subdirectories, hidden files and nonmatching extensions are skipped. Scanning
is limited to the named folder, with 64 ROMs per category and 256 total
game entries across at most eight categories. Empty/missing folders and
missing emulator binaries produce no visible category. The 64 KiB parser
input/output limits, 2048 tokens, 16 nesting levels, 255 bytes per string
and 48 launch arguments bound memory use. Malformed, duplicate-key and
truncated JSON is rejected; UTF-8 BOMs and Unicode escapes are supported.

An emulator definition is written once; adding another ROM needs no edit:

```json
{
  "system": ["--bright", "50", "--rotate", "2,0"],
  "categories": [],
  "emulators": [{
    "name": "NES",
    "bin": "fpbin/infones.bin",
    "directory": "games/nes",
    "extensions": [".nes"],
    "args": ["--scaler", "99", "--dir", "games/nes", "infones"]
  }]
}
```

Arguments are `[bin, ...system, ...args, ROM filename]`. Keep the `--dir`
argument consistent with `directory`; paths remain relative to the card
root. Manual port entries use `[bin, ...system, ...args]`. Only their binary
presence is checked; supply the configured WAD/GRP resources yourself.

The build pins fpdoom to `04f19d6d54430693d00029970c51cd988c083b05`,
stages sources under `build/fpdoom/fpmain-b310e`, applies `entry-json.patch`
to a private framework copy, and forces a full SC6530 (`CHIP=3`), SD-only
(`LIBC_SDIO=3`) rebuild. It never resets or overwrites the upstream app.
An existing clone at another revision is rejected; `FPDOOM_DIR` can select
a separate clone. Set `B310E_TOOLCHAIN` and, if needed, `B310E_HOST_CC` to
the compiler directories. Windows uses MSYS2 bash through the PS wrapper.

```powershell
$env:B310E_TOOLCHAIN = '/d/floppy/.tools/arm-toolchain/bin'
$env:B310E_HOST_CC = '/c/msys64/mingw64/bin'
& ./build.ps1 fpmain
# Host tests of the exact parser and argument packing:
& C:/msys64/mingw64/bin/python.exe ports/fpmain/tests/test-config.py `
  --cc C:/msys64/mingw64/bin/gcc.exe
# ARM startup, directory filtering, ROM selection and actual launch args:
& C:/msys64/mingw64/bin/python.exe ports/fpmain/tests/test-boot.py `
  --qemu D:/floppy/SM-B310E/build/qemu/build/qemu-system-arm.exe `
  --firmware D:/floppy/phonefirmware/e52q7a.bin `
  --toolchain D:/floppy/.tools/arm-toolchain/bin `
  --output emulator/qemu/logs/fpmain-boot
```

The ARM test uses generated FAT32 cards and an original 128 KiB launcher/ROM
fixture. It verifies JSON-only, empty, invalid and missing configurations,
then launches programs and a spaced ROM filename through the keypad and checks
the actual arguments and final payload word at the launched program. Add
`--pad-loader 1024` to repeat with a larger loader section. Symbol addresses
use the binary's own loader length rather than assuming a fixed offset.
The test supplies the loader's initial card
state/entry PC; it does not test NOR loader installation. Real-phone menu
boot still needs a retest. Copy the built `sdcard/fpbin/fpmain.bin` and
`config.json` to the card; the existing NOR loader need not change.

For a shared Rockbox/menu emulator card, generate FAT32 (fpdoom requires it):

```powershell
& C:/msys64/mingw64/bin/python.exe ports/rockbox/tests/make-sd-image.py `
  --fat32 --fpmain sdcard/fpbin/fpmain.bin --config sdcard/fpbin/config.json --force
```

This includes the menu, JSON config, `.rockbox`, `progs/rockbox.bin` and
`test.wav`; it does not package your games. The original default FAT16
image remains available for standalone Rockbox testing.


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

### Boot behavior

| Power-on | What happens |
|---|---|
| **any key held** | sdboot (`SDBOOTKEY=-1` in the header, `sdbootkey` = 0xffffffff) → chip init → reads `fpbin/fpmain.bin` from the card → copies to PSRAM `0x04000000` (NOR-boot window, MEM_REMAP=0) → jumps ARM-state → the fpmenu app runs its game menu |
| **no key** | sdboot chains to the original stock entry (`0x46e4`, stored at its header +4 by the install) — **normal stock boot, untouched** |

### What fpmain.bin is (and why our menu.bin is NOT it anymore)

The official fpdoom release builds `fpbin/fpmain.bin` from the **fpmenu** app
(`release.make`: `%/fpmain.bin: makebin fpmenu OPTS NAME=fpmain`). The app
format (verified against the stock build):

- **Example stock fpmenu build: 29684 B** = part2 stub (512 B) + part1 app
  (29032 B, linked at `0x14000000`, self-relocating via its appended reloc
  table) + relocs.
- The part2 length is stored in the word at file offset `+4`. It changes
  with the compiler: ARM GCC 14.2 produced 512 bytes and ARM GCC 15.3 produced
  504 bytes in the tested builds. fpmain reads that header at `0x04000000`,
  checks it against the part1 runtime address and the IRAM capacity, and copies
  the actual loader length. Invalid layouts show `LOADER ERROR` and return
  to the menu.
- sdboot loads the whole file to `0x04000000` (NOR boot) and jumps there.
  part2's `_start` (`bl __image_end`) branches into part1's `start.s`, which
  relocates itself for the `0x04000000` runtime base, then `entry_main` →
  `sys_init` (LCD/keypad from the firmware-dump scan) → `main()`.

**The `fpmain.bin` built by `build.ps1 fpmain` is the fpmenu skeleton with
our boot menu**
— **BOOT STOCK** + the card's **progs/** (OS-like / single apps, no args) +
the **fpbin/config.json ports and discovered ROMs**, launched with their
configured arguments.

**LAUNCH uses the fpdoom readbin mechanism:** part2's readbin is copied
to IRAM `0x40004000`, then reads the selected binary directly to the
NOR-boot PSRAM window `0x04000000` and jumps. A fpdoom app linked at
`0x14000000` applies its appended relocation table for that runtime base.
No memory remap or SMC reinitialization is needed. The explicit load target
avoids the SC6531E-only target computed by the original fpmenu.

**os.bin is now RELOCATABLE** (the fpdoom mechanism, `make os-sd`): linked at
`0x14000000`, built `-pie`, a pack_reloc `.rel` table appended, and
`arch/start.s` applies it (diff = runtime − link). It runs at `0x14000000`
via the readbin (diff 0), or self-relocates to `0x34000000` when USB-loaded
(`spd_dump fdl os.bin ram` — the SC6530 ram_addr, spd_dump.c:1319). The old
remap-cascade stub (`menu_stub.s`) is retired (gc'd out). The stock fpmenu is
kept as `ports/fpmain/fpmain.bin.stock-backup`.

The old `make fpmain` target (our `menu.bin` built as the payload) is
**obsolete**: it assumed sdboot loads the payload to `0x34000000`, but the
real loader copies to `0x04000000` (MEM_REMAP=0), where our menu cannot run.
Both `make menu`/`menu.bin` (the merged-boot menu) and `make fpmain` were
**removed from the Makefile 2026-08-28** — the boot menu is exclusively the
ported fpmain from `ports/fpmain/` (below).

### History — why "our menu.bin as fpmain.bin" never loaded

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

### SD card layout

```
fpbin/fpmain.bin   the sdboot payload = OUR boot menu (the ported fpmain,
                   built by build.ps1 fpmain)
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
generates there (entries with missing binaries are skipped).

### Build (the ported fpmain menu + the card)

The fpdoom clone lives at `build\fpdoom` (cloned automatically by
`build.ps1 fpmain` on first use; pinned revision above). The
loader itself is **not** built — we use the stock prebuilt `sdboot3.bin`
from the fpdoom release.

```powershell
# PATH needs: arm-none-eabi-gcc, gcc (WinLibs), make, and git bash for sh
$env:Path = "C:\Program Files\Git\usr\bin;" + $env:Path

# 1. the PORTED fpmain app (our boot menu on the fpmenu skeleton) — stages
#    sources into fpdoom\fpmenu, builds, copies to ports\fpmain\fpmain.bin +
#    sdcard\fpbin\fpmain.bin, restores the clone.
powershell -ExecutionPolicy Bypass -File build.ps1 fpmain

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
`bl __image_end` (ARM) → part1 at the offset stored in its `+4` size word.

### NOR install (REVERSIBLE — the flash step)

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

### Verification status

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

### Full-NOR backup / restore

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
