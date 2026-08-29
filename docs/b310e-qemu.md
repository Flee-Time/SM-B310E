# B310E QEMU machine - usage, boot paths, and bring-up state

The SC6530C QEMU machine (Wave 5) runs the stock Samsung SM-B310E firmware
(`tools/spd_dump/full-backup.bin`, 8 MiB NOR) and our own `os.bin` on the PC.
This document is the usage + boot-path reference. The machine sources live in
`tools/qemu-b310e/machine/` (copied into the qemu-src tree by
`tools/qemu-b310e/scripts/install-machine.ps1`; the build is the pinned
`qemu-src` clone at `<home>\qemu-b310e\qemu-src`). The work plan is
`.omo/plans/b310e-qemu-machine.md`; the append-only session log is
`.omo/notepads/b310e-qemu-machine/learnings.md` (READ FIRST for the root
causes behind every recipe below).

## Build

The full recipe is `tools/qemu-b310e/scripts/build-qemu.ps1` - idempotent,
runs from plain PowerShell 5.1:

1. **Clone-or-pin** - if `<home>\qemu-b310e\qemu-src` is missing,
   shallow-clone `https://gitlab.com/qemu-project/qemu` at tag `v11.1.0`
   (pin log `tools/qemu-b310e/logs/qemu-pin.txt`; commit
   `84f07211cc5b4fc6a371559bf8a5de4fb068e648`); if present, verify
   `git describe --tags` pins `v11.1.0*` (a wrong tree aborts instead of
   clobbering).
2. **Install the machine** - runs `install-machine.ps1`, which copies
   `machine/hw/arm/*.c` -> `qemu-src/hw/arm/` and `machine/hw/misc/*.c` ->
   `qemu-src/hw/misc/` and appends marker-guarded meson.build/Kconfig wiring
   (generated from the files actually present - dropping a new `.c` file and
   re-running the script wires it automatically; a re-run is a no-op).
3. **Configure** - `./configure --target-list=arm-softmmu --enable-png
   --disable-werror` (PNG is REQUIRED for `screendump`; the libpng package is
   MSYS2 `mingw-w64-x86_64-libpng`). Fingerprint-gated: skipped when the
   build dir matches the recorded args.
4. **Build** - `make -j<N>` inside the MSYS2 MINGW64 shell.
5. **Sanity** - the exe prints `QEMU emulator version 11.1.0` and
   `CONFIG_PNG=y` is confirmed.

```powershell
# full build (elevated PowerShell, or add -AutoElevate):
powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\build-qemu.ps1
# partial / dry runs:
powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\build-qemu.ps1 -WhatIf
powershell -NoProfile -ExecutionPolicy Bypass -File tools\qemu-b310e\scripts\build-qemu.ps1 -SkipConfigure -SkipBuild
```

The exe ends up at `<home>\qemu-b310e\qemu-src\build\qemu-system-arm.exe`,
self-contained (the 15 MSYS2 runtime DLLs are copied next to it - no PATH
surgery needed).

**Elevation is mandatory for the build** (the todo-8 lesson, WinError 1314):
meson's postconf `symlink-install-tree.py` calls `os.symlink()`, which fails
with `OSError: [WinError 1314]` unless the token has
SeCreateSymbolicLinkPrivilege (run elevated / UAC via `-AutoElevate`). Two
sub-lessons:
- `msys2_shell.cmd` FAILS when launched elevated - bypass it with
  `set MSYSTEM=MINGW64` + `C:\msys64\usr\bin\bash.exe -lc "..."` inside a
  `.cmd` wrapper launched `Start-Process -Verb RunAs -Wait` (the build
  script does this automatically).
- **The non-elevated incremental trick**: a `.c`-only change in the machine
  sources rebuilds NON-elevated (`make -j8` from the MINGW64 shell directly,
  MAKE_EXIT=0). Elevation is only needed when meson regenerates (new
  files/Kconfig). Every wave session after Wave 3 rebuilt this way; the
  elevation is the configure/relink exception, not the rule.

MSYS2 package set (`base-devel binutils bison diffutils flex git grep make
sed mingw-w64-x86_64-toolchain mingw-w64-x86_64-glib2
mingw-w64-x86_64-pixman mingw-w64-x86_64-pkgconf mingw-w64-x86_64-ninja
mingw-w64-x86_64-python mingw-w64-x86_64-libpng`; SDL2/gtk3 NOT needed -
headless via screendump). Windows Developer Mode ON (os.symlink
privilege). Package list evidence: `tools/qemu-b310e/logs/msys2-packages.txt`.

**Run-time contract learned the hard way**: the guest's IRQ delivery and the
model's `qemu_log` evidence BOTH depend on `-D <logfile>` - without `-D`,
`qemu_log` falls back to stderr, the vCPU thread blocks when the redirected
pipe fills (~64 KB, the 1 kHz tick flood), and the guest never completes its
ISR (the tick dies silently). **Always pass `-D <logfile>`.** Likewise
`screendump` writes PNG only when `-f png` is passed EXPLICITLY - the `.png`
filename extension is never consulted (default is PPM). See the trace
section for the exact spellings.

## Run

The machine: `-M b310e,boot-mode=warm|stock|ours` (default warm) with
`-display none -serial none` (headless; the display renders through the
QEMU console, captured with `screendump`). Options:

- `boot-mode` - `warm` (default), `stock`, or `ours`.
- `hold-end=on` - assert the EIC END key (0x82001900 bit 3) from reset - the
  END-hold test (FALSIFIED, see Boot-paths).

The `-drive` lines:
- **NOR** (boot=warm/stock): `tools\spd_dump\full-backup.bin` - the
  AUTHORITATIVE 8 MiB single-NOR stock backup (8/23). `dump_firmware.bin`
  (repo root, 16 MiB) is an exact 2x self-mirror of the same NOR - all
  firmware regions are byte-identical (PBL @0x0 / entry 0x46e4, main OS
  @0x10000, DSP_CODE @0x570000, NV audio @0x680000, FS @0x690000,
  dsp_codec @0x876e0); only the user-data area (0x6B0000+ FAT12 partitions)
  differs. Use full-backup.bin (exact 8 MB, no mirror waste).
- **os** (boot=ours): `tools\qemu-b310e\logs\os-sdboot-era.bin` - the
  COMMITTED sdboot-era os.bin (18,094 B, entry 0x34000010, fixed-linked
  0x34000000). **The CURRENT working-tree os.bin (the user's relocatable
  `-pie` WIP built by `make`) is NOT bootable in the emulator**: it
  data-aborts in `_start` (bss_start 0x34005096 unaligned - alignment fault
  with the CP15 A-bit on; plus the two +0x10 relocation errors). Always use
  the committed `logs/os-sdboot-era.bin` for boot=ours.

### warm - the stock fast path (PC = 0x10000)

```powershell
%USERPROFILE%\qemu-b310e\qemu-src\build\qemu-system-arm.exe `
  -M b310e,boot-mode=warm -display none -serial none -d int `
  -D tools\qemu-b310e\logs\warm.log `
  --trace sc6530_ana_* --trace sc6530_aux_* --trace sc6530_dsp_cmd `
  -monitor telnet:127.0.0.1:4546,server,nowait `
  -drive file=tools\spd_dump\full-backup.bin,format=raw,if=none,id=nor
```

Boots the stock main OS directly (no PBL): the `0xFE519C04` warm-boot magic
is written to the PSRAM base and the CPU starts at 0x10000. With the full
overlay chain (see Limitations) this reaches the display phase, the audio
bring-up burst, and the periodic DSP keepalive cycle.

### stock - the full PBL (PC = 0x0)

```powershell
%USERPROFILE%\qemu-b310e\qemu-src\build\qemu-system-arm.exe `
  -M b310e,boot-mode=stock -display none -serial none -d int `
  -D tools\qemu-b310e\logs\stock.log `
  --trace sc6530_ana_* --trace sc6530_aux_* --trace sc6530_dsp_cmd `
  -monitor telnet:127.0.0.1:4547,server,nowait `
  -drive file=tools\spd_dump\full-backup.bin,format=raw,if=none,id=nor
```

Runs the full PBL (boot vector 0x0 -> 0x46e4 -> mode-stack setup -> full
init -> the NOR->PSRAM scatter reload -> main OS). NOTE `-d int` (NOT
`-d in_asm`) for timing-sensitive stock runs: in_asm makes the PBL phase
~100x slower and floods the log (multi-GB). The full PBL touches the SAME
overlay gates as warm (see Boot-paths divergences below).

### ours - our os.bin (PC = 0x34000010)

```powershell
%USERPROFILE%\qemu-b310e\qemu-src\build\qemu-system-arm.exe `
  -M b310e,boot-mode=ours -display none -serial none -d int `
  -D tools\qemu-b310e\logs\ours.log `
  --trace sc6530_lcdc_refresh `
  -monitor telnet:127.0.0.1:4548,server,nowait `
  -drive file=tools\spd_dump\full-backup.bin,format=raw,if=none,id=nor `
  -drive file=tools\qemu-b310e\logs\os-sdboot-era.bin,format=raw,if=none,id=os
```

The testbed run: os.bin boots (banner ~10-12 s), the 1 ms tick runs
(~950 IRQs/s), and sendkey drives the keypad. The bounded USB connect wait
burns its 10M-iteration budget in <1 s under `-d int` and proceeds. **Use
`-d int`, never `-d in_asm`** for evidence runs: under in_asm every
iteration of the 10M-budget poll logs a ~1.1 KB register dump (~4 min wall
per budget, ~11 GB) - the full boot to banner takes ~8 min / ~20 GB.

The reusable run drivers (evidence-proven, fix the sleeps only):
`tools/qemu-b310e/logs/w7/run-w7-ours.ps1` (boot=ours banner + keys) and
`tools/qemu-b310e/logs/w6/run-w6-stock-sound.ps1` (boot=warm audio capture).

## Memory map

All real regions sit ABOVE the catch-all (priority rule: catch-all at
priority 0, devices/aliases at 1, DSP APB subregisters + the PSRAM overlay
chain at 2 - the catch-all only ever sees addresses nothing claims).

| Address | Size | Region / device | Model |
|---|---|---|---|
| 0x00000000 | 8 MiB | NOR XIP | read-only RAM from `id=nor`; writes LOGGED, not stored |
| 0x04000000 | 4 MiB | PSRAM alias | always-on alias of the 0x34000000 RAM (MEM_REMAP no-op) |
| 0x10000000 | 128 KiB | DSP shared RAM | fake handshake RAM; control windows at base+0xFE0 |
| 0x30000000 | 128 KiB | DSP shared RAM alias | defensive (SC6531EFM-family base; the plan's 0x30000000 NOR alias was DROPPED) |
| 0x34000000 | 4 MiB | PSRAM (machine RAM) | boot=ours os.bin loads here |
| 0x40000000 | 256 KiB | IRAM | RAM (vectors/stack) |
| 0x20000000 | | SMC | aux log+store bank |
| 0x20300000 / 0x20380000 | 0x1000 / 0x100 | USB controller + FIFO window | no-op ("host never connected") |
| 0x20400000 | 0x3000 | BUS_MON | aux silent store+echo |
| 0x20500000 | 0x1000 | AHB | aux log+store; 0x205003fc reads chip-id 0x6530c000; 0x205000e0 MEM_REMAP = logged no-op |
| 0x20700000 | 0x1000 | SDIO0 | no-op ("no card") |
| 0x20800000 | 0x1000 | LCM DBI | store+echo (panel table not modeled) |
| 0x20a00000 | | SFC | NOT a device: catch-all benign entry, SFC_STATUS 0x20a00010 = 0x3 (ready\|idle) |
| 0x20d00000 | 0x1000 | LCDC | QemuConsole 128x160; refresh = ctrl bit 3 -> copy fb (img.y_base_addr << 2) |
| 0x80000000 | | INTC | pending 0x04 (raw levels), enable 0x08, disable 0x0c; CPU IRQ = (level & enabled) |
| 0x81000040 | | timer2 | 1 ms ptimer -> INTC line 23; +0x0 LOAD, +0x8 CTL 0xc0, +0xc INT (write 9 = clear + keep enabled) |
| 0x81003000 | | sys-timer | +0x4 SYS_CNT0, +0x8 SYS_CTL, +0xc SYS_MS 1 ms counters (write-tolerant) |
| 0x82000000 | 0x1000 | ADI mailbox | +0x18 RD_CMD, +0x1c RD_DATA (index echo), +0x20 STS = 0x100 |
| 0x82001000 | 8 KiB | ANA analog bank | codec regs + WDG 0x82001480 + EIC 0x82001900; sc6530_ana_read/write trace |
| 0x82003000 | 0x100 | VBC | audio FIFO (separate region - a single 0x2000 ANA ends at 0x82002fff) |
| 0x87000000 | 0x40 | keypad matrix | edge-latch (int_raw 0-3 down / 4-7 up); END via the EIC bit in the ANA bank |
| 0x8a000000 | | GPIO | aux log+store bank |
| 0x8b000000 | 0x3000 | APB | aux log+store bank + the DSP device's 4-byte APB subregisters (0x8b000140/160/1a0/1c0/1c4/1068/2068) |
| 0x8c000000 | | pinmux | aux log+store bank (SAFE here - the real 0x8c region is the banned/hang territory) |
| 0x10000000..0xffffffff | | catch-all | priority 0: UNMODELED r/w log, reads 0, benign-ready table for known polled status regs |

Note: the display refreshes from PSRAM through the always-on 0x04000000
alias, so the framebuffer is readable whether the guest computes it in the
0x34000000 or the 0x04000000 window.

## Trace / screendump / key injection

### Trace flags

- **ONE pattern per `--trace` flag.** The comma-list form
  `--trace a,b,c` KILLS qemu at startup (parses as separate boolean
  options, "Invalid parameter 'sc6530_aux_*'"). The audio capture uses
  three separate flags:
  `--trace sc6530_ana_* --trace sc6530_aux_* --trace sc6530_dsp_cmd`
  (all three match, together with `-D`, the audio observatory).
- `--trace sc6530_lcdc_refresh` - the LCDC display refresh (the main-loop
  display-update evidence: `sc6530_lcdc_refresh LCDC refresh fb=0x...`).
- The `sc6530_ana_read/write` events carry the guest PC; read-data traces
  resolve the ANA address from the mailbox index (the last 0x82000018
  write), not the 0x8200001c MMIO offset - the Wave-6 diff semantic.
- The `sc6530_dsp_cmd(id, arg, pc)` event carries the DSP-fake command
  stream (REG_WRITE/REG_READ/OWNERSHIP on the audio-ownership register
  0x8b0001c4 etc.).

### screendump (HMP)

`-f png` is REQUIRED (default format is PPM; the `.png` filename extension
is never consulted) and the FILENAME comes FIRST:

```
screendump screen1.png -f png
```

A black framebuffer compresses to a valid <1 KB PNG (do not gate on file
size for solid-color screens).

### sendkey (key injection)

The keypad device registers a QEMU input handler; v11.1.0 delivers LINUX
KEY_* codes in the input events (not QKeyCodes), and HMP `sendkey`
accepts QKeyCode names or raw hex. **`sendkey enter` does NOT parse - the
QKeyCode enum names Enter `ret`.** Mapping:

| sendkey | B310E key | code |
|---|---|---|
| `ret` | CENTER | 0x0d |
| `kp_enter` | DIAL | 0x01 |
| `1`..`9` `0` | digits | 0x31..0x39 / 0x30 |
| `minus` `asterisk` | STAR | 0x2a |
| `slash` `kp_slash` | HASH | 0x23 |
| `f1` / `f2` | LSOFT / RSOFT | 0x08 / 0x09 |
| `up` `down` `left` `right` | d-pad | 0x04..0x07 |
| `esc` `end` | END (EIC bit 3, not the matrix) | - |

A `sendkey` press produces a DOWN/UP edge pair in `int_raw` (bits 0-3 down,
4-7 up) plus the key_status row byte; the guest ACKs with an int_clr write.
END toggles EIC bit 3 through the ADI ANA bank (visible to the guest's
mailbox read path).

### The TcpClient monitor pattern

The reusable HMP driver (w6/w7 run scripts, `Wait-Port` + `HMP` functions):
connect a `System.Net.Sockets.TcpClient` to `127.0.0.1:PORT`, write the
command as an **LF-terminated ASCII line**, read the reply, and close the
connection after every command. Do NOT use CRLF (qtest-style chardevs
reject it; HMP is tolerant but LF-only keeps one code path). Kill zombie
qemu processes BEFORE re-running (a stale qemu holds the chardev port and
your script silently talks to the STALE guest). Useful HMP: `screendump`,
`xp /4wx <addr>` (physical read - e.g. `xp /4wx 0x20d00024` for the LCDC
img base, `xp /4wx 0x8100300c` for the sys-ms counter), `info registers`,
`trace-event <name> on|off` (runtime trace toggling), `quit`.

## Boot-paths

The stock firmware boots two ways, selected by `boot-mode`.

### warm - `-M b310e,boot-mode=warm` (default; PC = 0x10000)

Skips the PBL entirely: the CPU starts at the main-OS vector table (0x10000),
the warm-boot magic (`0xFE519C04`) is written to the PSRAM base, and the OS
runs straight through the boot task -> module init -> the display phase.

| Overlay / fix | Why the warm path needs it |
|---|---|
| `bootready` 0x0425de8c = 1 | the boot-task gate flag (the warm-path setter never runs) |
| `timerobj` 0x0422d2cc = 0x0422e3d0 | the timer-ops pointer (the init chain 0x111758->...->0x16c4c never runs) |
| `scirpos` 0x0422d4b4 cap 9 | the SCI IRQ-nesting position lockstep (the ENTRY asserts at 10) |
| `dlofitbl` 0x0422e330 = 0x04230a74 | the dl_ofi driver-table pointer |
| `txkern`/`txobj` 0x0422c654/0x0423c818 = 0x20021201 | the ThreadX kernel-struct magic the RTOS APIs check |
| `lcdtbl`/`lcddrv` 0x0422c8ec/0x0422c8fc | the LCD driver table -> the 128x160 panel descriptor (NOR 0x0CB414) |
| `clkobj`/`clkobjflag` 0x0422e554/0x0422e57c | the USB vcom device thiz + the +40 flag - breaks the ~40 s reboot loop (the ClkObj dispatcher's second-level `blx [[thiz+0]+0x2c]` -> vector-table indirection -> PBL reboot) |
| `scipooltbl`/`scipool`/`scimem` 0x0422d478/0x04280000/0x04280040 | the SCI_BLKMEM pool (the "PEAK" magic + memory-space) - the sci_mem "ASSERT: Error 0xff" pool-alloc crash (the pool table was all zeros) |
| `txstate1` 0x0422c67c = 0xF0F0F0F0 | the ThreadX kernel-state gate - the "Invalid caller of this service" (Error 0x13) |
| `txguard` 0x04259620 = 0 (absorb-all) | the dlmalloc mspace guard - the threadx_malloc.c:1274 ASSERT(0) (the ~40 s soft-restart reload copied junk over the guard + mstate) |
| `txpool`/`txnode`/`txsent` 0x04259638/0x04259700/0x0425a708 | the ThreadX byte-pool (tx_byte_allocate WAIT_FOREVER variant) |
| SFC_STATUS 0x20a00010 benign = 0x3 | the SFC driver waits for bit 0 (ready); value 2 (bit 0 clear) spun the poll forever |
| `hold-end` (optional) | the EIC END-key physical-level model (the END-hold test - FALSIFIED, see below) |

### stock - `-M b310e,boot-mode=stock` (PC = 0x0)

Runs the full PBL (boot vector 0x0 -> 0x46e4 -> the mode-stack setup ->
caseD_a -> the reload). The PBL touches extra init the warm path skips:

- **SMC** (0x20000000): ~31 writes (vs 15 warm, so **+16**) - the PBL
  programs the SDRAM controller timing from IRAM.
- **DSP** (0x8b0001a0/0x140): ~4 (vs 2 warm, so **+2**) - the DSP
  MCU_CTL0 / INT_STS.
- **pinmux** (0x8c0001xx): ~203 (vs 198 warm, so **+5**) - the PBL walks
  the pinmux registers (log+store in the emulator; the real 0x8c region is
  the banned/hang territory - the emulator is safe).
- The **NOR -> PSRAM reload** (the scatter copy of the main-OS image +
  data).

The stock path converges on the SAME main OS and needs the SAME overlay gates
(the boot-state overlays are needed on BOTH paths - the warm-path ThreadX
kernel-init gap is present on the stock path too: the full PBL does NOT run
the ThreadX/device-create chains). Both paths reach the display phase with the
same result (see the bring-up state below).

### END-key finding (FALSIFIED on both emulated paths)

The user HW finding was that the stock boot waits for the END key held. The
emulator test (a `hold-end` property + the EIC physical-level model) shows
NEITHER path waits for the END key early:

- The guest's EIC reads (mailbox index 0x900) happen only LATE (the
  battery/charging monitor, interleaved with the ADC reads) - there is NO
  early boot key-wait.
- The guest's EIC_DBNC_DMSK enables channels 2+4 (0x14), NOT channel 3 (the
  AGENTS.md END channel) - the END bit (3) stays masked.
- The overlay-off experiment (all PSRAM overlays disabled + END held) stalls
  EARLIER (the boot-task DSP wait) - the overlays are NOT masking a key-gated
  boot.

### SFC status fix

The SFC driver (NOR 0x1194xx) waits for `SFC_STATUS & 1` (the ready bit) in
every operation (`FUN_0011a188/0011a27a/0011a4cc`:
`do { FUN_0011954c(); } while ((status & 1) == 0)`). The model's benign answer
for 0x20a00010 is **0x3** (bit 0 = ready | bit 1 = idle) - with the earlier 2
(bit 0 clear) the OS spun the poll forever (526k reads/55s).


### Keyboard Map

With a live display window (GTK/SDL), you can use your host keyboard to press phone keys. Headless `sendkey` commands use the QEMU QKeyCode names (e.g. `ret`).

| Host Key (sendkey) | Phone Key | Matrix (Row, Col) |
| --- | --- | --- |
| Enter (`ret`) | CENTER | Row 0, Col 0 |
| Keypad Enter (`kp_enter`) | DIAL | Row 0, Col 1 |
| 0-9 (`0`-`9`) | 0-9 | Various |
| Minus (`minus`) / Asterisk (`asterisk`) | STAR (*) | Row 4, Col 0 |
| Slash (`slash`) / Keypad Slash (`kp_slash`) | HASH (#) | Row 4, Col 2 |
| F1 (`f1`) | LSOFT | Row 1, Col 3 |
| F2 (`f2`) | RSOFT | Row 2, Col 3 |
| Up/Down/Left/Right (`up`/`down`/`left`/`right`) | D-pad | Various |
| Esc (`esc`) / End (`end`) | END / Power | EIC Channel |

## Limitations

What the model does NOT do, and what that means for a guest (the plan's
list + everything learned since):

- **DSP is faked at the interface level.** No DSP-core emulation (the DSP
  firmware is proprietary-ISA). The fake answers the share-mem handshake per
  `docs/audio-dsp-protocol.md` section DSP-fake-spec: control windows at
  0x10000FE0/0x30000FE0 (the guest's actual base, runtime
  [0x0422E598] = 0x10000FE0), 7 APB subregisters, ready=1 immediately, ack
  every command, log all unanswered traffic. The stock OS never runs the
  DSP download protocol (no SM_BOOT/BLOCK/DATA/COPY/RUN) - only REG_*,
  OWNERSHIP and arm_ctl keepalive traffic, all answered.
- **USB is a no-op**: log+store; status reads return 0 = "host never
  connected" forever. os.bin's USB waits are BOUNDED (10M / 2M budgets) and
  burn out and return; no libc_server connection is possible (see the
  bridge item below).
- **SD is absent**: SDIO0 is log+store; status reads return 0 = "no card".
  os.bin's SD probe reports no card and moves on (bounded waits).
- **Flash writes are logged, not stored**: NOR is read-only XIP RAM; the
  guest's flash/NV writes appear in the log with the guest PC and are
  dropped.
- **MEM_REMAP is a no-op**: the PSRAM alias at 0x04000000 is always on;
  writes to 0x205000e0 are logged and ignored. This is what lets the stock
  OS (which computes its framebuffer in the 0x04000000 window on real HW)
  render.
- **The stock-OS warm-path bring-up overlay chain** (19 PSRAM regions, the
  table in section Boot-paths). The stock OS's master init chain
  (0x111758 -> trampoline 0x11d8b0 -> 0x16c4c -> the ThreadX/device-create
  chains) has no static caller on the boot paths the emulator runs, so the
  OS hits one uninitialized-state gate after another: the boot-ready flag,
  the timer-ops pointer, the SCI IRQ-nest cap, the dl_ofi driver table, the
  ThreadX kernel magics, the LCD driver table, the USB vcom thiz (the ~40 s
  software-reboot loop), the SCI PEAK pool, the ThreadX kernel-state word,
  the dlmalloc guard (threadx_malloc.c:1274 assert) and the byte pool.
  Each gate was root-caused from the guest's PC/assert and modeled as an
  overlay that absorbs the BSS-memset zero-writes while answering the
  polled value. The END-key-hold hypothesis was FALSIFIED on both paths (no
  early EIC read; channel 3 never unmasked; overlay-off stalls EARLIER).
- **The AST_BLUESCREEN / threadx_malloc asserts**: Wave 5's residual ~34
  recoverable "AST_BLUESCREEN" assert headers (~1 per 2 s, guest recovers +
  continues) were the display-era noise of the pre-malloc-fix state. The
  Wave-6 malloc-fix (txguard + byte-pool overlays, per-word validated
  writes) eliminated the 1274 assert screen entirely - the current runs show
  ZERO "Taking exception" and no new assert gate.
- **Display state**: the stock OS renders frames (LCDC refresh fires,
  screendumps non-black) but the img-base setter (FUN_00027e38 ->
  0x20d00024) runs in the MAIN LOOP, which the warm path never reaches -
  so 0x20d00024 stays 0 and the stock screendumps are framebuffer-noise
  (the earlier "green fill" was the threadx_malloc assert screen, since
  fixed). Our os.bin banner WORKS (fb=0x340054c8, black bg + white 5x7
  text; 116 LCDC refreshes; tick ~950/s; keys acknowledged).
- **The stock OS never reaches a key-processing/UI phase on the warm
  path**: the Wave-6 key-injection run (15 sendkeys) produced NO
  key-triggered audio burst - the best captured sound event is the
  early-boot audio bring-up burst (P1-P7: codec-ID read, AHB gates, the
  23-bit 0x8b0000a0 APB power ladder, the ANA clock block
  0x820010e0/0x10e4/0x1040, the pinmap + ANA pads 0x82001850..0x1884, GPIO)
  plus the periodic DSP keepalive cycle.
- **The VBC FIFO was never reached on stock paths**: the sound-DATA path
  (VBC FIFO 0x82003018 writes) stops before VBC - only ARM-side config and
  keepalive run. The VBC DMA-EN bits / power half / ARM_VB_ACC fix in the
  findings doc is protocol-doc-derived, not capture-derived.
- **gdb breakpoints do NOT stick on this guest** (remote-stub overhead +
  PSRAM); use the monitor (`xp`, `screendump`) for evidence instead.
- **libc_server-over-socket bridge = future item** (explicitly out of scope
  for this plan): os.bin's USB console cannot be reached over the network
  until someone bridges the USB no-op model to a socket.
- **Pinmux is safe here**: the stock OS's 0x8c pinmap replay (incl. the
  BANNED-on-HW 0x8c0002a4=0x231) executes harmlessly in the emulator - this
  is a feature, not a limitation. On real HW the 0x8c region stays banned.


### Stock-OS Boot Investigation (Part 3)

We investigated why the stock firmware on the `warm` and `stock` boot paths never configures the LCDC framebuffer (i.e., `0x20d00024` stays `0`).

**Hypothesis**: The stock ST7735S panel driver reads the panel ID via the LCM DBI data window (writing `0x04` RDID command to `0x60000000`, then reading 3 bytes from `0x60020000`). If it reads `0` (which is what the catch-all region returned), the driver bails out and skips display initialization entirely, meaning `FUN_00027e38` is never called.

**Machine Change**: We introduced an `sc6530_lcm` data window memory region at `0x60000000` (size `0x40000`). It implements a state machine that answers the `0x04` RDID command with the expected sequence for an ST7735S panel (`0x00` dummy, `0x7c`, `0x89`, `0xf0`).

**Firmware Mismatch**: When verifying this with a publicly available FSPD firmware (Samsung_Guru_Music_2_SM-B310E_FSPD), we noted an **address mismatch**. The FSPD firmware is a different build/version than the user's original dump. While landmarks like `0x04259620` and `0x0422d4b4` align, the LCD driver table pointer (`0x000cb414`) provided by the `sc6530_aux` overlay points to invalid data (`0x0000002e`) in the FSPD firmware. Consequently, the FSPD firmware bails out early during the LCD init phase before ever attempting to read the panel ID from `0x60000000`.

**How to verify**: Users with the original `dump_firmware.bin` that matches the machine's `sc6530_aux` overlays can run the following test locally:

```bash
./tools/qemu-b310e/scripts/run-b310e.sh -Boot stock -D tools/qemu-b310e/logs/stock.log
# Or on Windows:
# powershell -File tools/qemu-b310e/scripts/run-b310e.ps1 -Boot stock -D tools/qemu-b310e/logs/stock.log
```

Look for writes to `0x20d00024` in the `stock.log` or examine the final screendump PNG. If the RDID response satisfies the original panel driver, the UI/key phase should start and `0x20d00024` should be configured.


## Bring-up state (Wave 5)

- The display RENDERS: the LCDC refresh fires (`sc6530_lcdc_refresh LCDC
  refresh fb=0x4076e00 pc=0x00027dbc` - the img base is set and the
  display-update code triggers the DMA refresh). Screendumps:
  `tools/qemu-b310e/logs/w5/screen-warm.png` and `screen-stock.png` (both
  128x160, non-black - a green framebuffer fill).
- Known residual (Milestone D, documented not chased): ~34 recoverable
  "AST_BLUESCREEN" assert headers (~1 per 2 s; the guest recovers + continues -
  the display + the 1 ms timer + the bootready polls run) and ~707 core-path
  UNMODELED reads (mostly the debug UART 0x8400000c poll x511 + the SFC
  driver's other 0x20a000xx reads).
- Audio (Milestone C): the codec ANA writes are captured; the DSP answers the
  REG_READ/REG_WRITE handshake.

### Wave 6/7 addendum

- The Wave-6 malloc-fix removed the green "assert screen" (the 1274
  threadx_malloc ASSERT); current runs show zero exceptions, the dlmalloc
  carve is live through the validated overlays, and the display-update code
  runs (lcdc_refresh pc=0x27dbc) with 0x20d00024 still 0 (the known main-loop
  gap).
- boot=ours (Wave 7) is the durable testbed: banner, ~950 IRQ/s tick, all
  mapped keys edge-acknowledged, zero UNMODELED lines, zero aborts - evidence
  in `tools/qemu-b310e/logs/w7/`.

## Known model conventions

- The NOR is direct XIP RAM; the SFC (SPI flash controller) is "permanently
  ready" (benign 0x3).
- The PSRAM boot-state overlays (the table above) sit OVER the PSRAM alias at
  priority 2; the catch-all (0x10000000..0xffffffff) is priority 0.
- gdb breakpoints do NOT stick on this guest (the remote-stub overhead +
  PSRAM); use the monitor (`screendump`, `xp`) for evidence instead.
