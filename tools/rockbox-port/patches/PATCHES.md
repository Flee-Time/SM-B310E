# B310E Rockbox port — integration patches (T2.1 deliverable)

**Date:** 2026-08-27 · **Applies to:** the Rockbox clone at
`%TEMP%\opencode\rockbox` (master @ `0e2a3cc`).

These are the 5+ integration edits the build task (T2.2) applies BEFORE
running `tools/configure`. Each is documented with exact context + the
insertion. The overlay target files live under `firmware/…` / `apps/…` in
this directory and are staged into the clone at the matching paths.

---

## 0. Overview of the model chain (how a new target wires in)

- **`config.h`** includes the per-model header: `#elif defined(B310E)`
  → `config/b310e.h`. That header defines MODEL_NUMBER 126 and the
  CONFIG_* values.
- **`cpu.h`** includes the SoC header: `#elif CONFIG_CPU == SC6530C`
  → `sc6530c.h` (FRAME/TTB/register bases).
- **`audiohw.h`** includes the codec/settings header: `HAVE_SC6530_CODEC`
  → `audiohw-sc6530c.h` (the AUDIOHW_SETTING table; sound.c generates the
  `_audiohw_setting_*` structs from it).
- **`firmware/SOURCES`** lists the target files under `#ifdef B310E`.
- **`apps/SOURCES`** lists the keymap under `#elif CONFIG_KEYPAD ==
  B310E_PAD`.
- **`tools/configure`** selects the target and drives the toolchain.

Free ID values (checked against master @ 0e2a3cc — these are #define lists,
not C enums):

| Macro | Value | Notes |
|---|---|---|
| `MODEL_NUMBER` | **126** | highest in use = 125 (hidizsap80max.h); values are NOT unique upstream |
| `SC6530C` (CONFIG_CPU) | **6530** | highest numeric = 32743 (STM32H743); 6530 collision-free |
| `B310E_PAD` (CONFIG_KEYPAD) | **81** | highest in use = 80 (HIDIZS_AP80MAX_PAD) |
| `LCD_B310E` (CONFIG_LCD) | **73** | highest in use = 72 (LCD_ECHO_R1) |
| `RTC_SC6530` (CONFIG_RTC) | **28** | highest in use = 27 (RTC_STM32H743) |
| `STORAGE_SD` | existing bit 2 | CONFIG_STORAGE is bit flags; no new value needed |

---

## 1. tools/configure — add the `b310e` target

**Menu line** (target-selection ASCII list; the Echo R1 area is a good
anchor — grep `"270) Echo R1"`). Add a line for 271:

```
271) B310E (WIP)
```

**Case entry** — add a new `271|b310e)` case. Model it on the echor1
entry (configure L4101-4123) and the mini2440 entry (removed 2026-02-03;
parent SHA `1e2950cc…`). Insert anywhere in the case chain (before the
`*)` fallback):

```
    271|b310e)
    target_id=126
    modelname="b310e"
    target="B310E"
    memory=4
    arm926ejscc
    appextra="recorder:gui"
    plugins=""
    tool="cp "
    boottool="cp "
    output="rockbox.bin"
    bootoutput="bootloader-b310e.bin"
    toolset=$scramblebitmaptools
    t_cpu="arm"
    t_manufacturer="sc6530c"
    t_model="b310e"
    ;;
```

Notes:
- `memory=4` → `-DMEMORYSIZE=4` (the 4 MB PSRAM). HAVE_DIRCACHE auto-off
  (<8), crossfade/perceptual-volume on (4).
- `arm926ejscc` (configure L576) → `-mcpu=arm926ej-s`, ARM state. The
  compiler defines `__ARM_ARCH` = 5; `__ARM_ARCH_PROFILE` is undefined so
  configure falls back to `arch_profile=classic` → `CPU_ARM_CLASSIC`.
- `tool="cp "` / `boottool="cp "` → raw image, no scramble → the B310E
  boot menu can load it directly.
- `plugins=""` → PLUGIN_BUFFER_SIZE is still honored (config/b310e.h);
  no plugin binaries are built for M1.
- `target_id` must be unique; 126 matches MODEL_NUMBER. If configure
  already uses 126 (check `grep target_id=126`), pick the next free.

---

## 2. firmware/export/config.h — 5 edits

### 2a. CONFIG_CPU list (L57-85, `#define`-based)

Add `SC6530C` to the CONFIG_CPU block (alphabetical-ish; place after
`S3C2440` on L65 or at the end of the block on L85):

```c
#define S3C2440      2440
#define SC6530C      6530
```

### 2b. CONFIG_KEYPAD list (L97-160)

Add at the end of the block (after `HIDIZS_AP80MAX_PAD` on L160):

```c
#define B310E_PAD        81
```

### 2c. CONFIG_LCD list (L215-276)

Add at the end of the block (after `LCD_ECHO_R1` on L276):

```c
#define LCD_B310E       73
```

### 2d. CONFIG_RTC list (L328-353)

Add at the end of the block (after `RTC_STM32H743` on L353):

```c
#define RTC_SC6530      28
```

### 2e. Model include chain (L584-588, the tail)

Insert `#elif defined(B310E) …` immediately BEFORE the `#else` on L586:

```c
#elif defined(HIDIZS_AP80MAX)
#include "config/hidizsap80max.h"
#elif defined(B310E)
#include "config/b310e.h"
#else
#error "unknown hardware platform!"
#endif
```

---

## 3. firmware/export/cpu.h — SoC header include (L37-38 chain)

Add a `SC6530C` branch (before the `#endif` on L63):

```c
#elif CONFIG_CPU == STM32H743
#include "stm32h743.h"
#elif CONFIG_CPU == SC6530C
#include "sc6530c.h"
#endif
```

---

## 4. firmware/export/audiohw.h — codec/settings header (L162+ chain)

The chain picks a codec header by a `HAVE_*_CODEC` define; config/b310e.h
defines `HAVE_SC6530_CODEC`. Add the branch (before the chain's final
`#else`/`#error` — insert after L206 `dummy_codec.h` line if present, or
anywhere in the chain):

```c
#elif defined(HAVE_SC6530_CODEC)
#include "audiohw-sc6530c.h"
```

This header (staged at `firmware/target/arm/sc6530c/audiohw-sc6530c.h`)
declares `AUDIOHW_CAPS (MONO_VOL_CAP)` and `AUDIOHW_SETTING(VOLUME, …)`;
sound.c's `AUDIOHW_IS_SOUND_C` pass generates the `_audiohw_setting_VOLUME`
struct. The target dir is on the include path (`TARGET_INC="-I$(FIRMDIR)/
target/$t_cpu/$t_manufacturer/$t_model"` configure L4845 + L4860), so the
quoted include resolves.

---

## 5. firmware/SOURCES — target file list

### 5a. mmu-arm.S list (L671-675) — add SC6530C

`mmu-arm.S` provides the ARM cache ops (`commit_dcache`, `commit_discard_*`)
that `lcd-sc6530c.c` calls. Add `|| CONFIG_CPU == SC6530C` to the
condition:

```c
#  if CONFIG_CPU == IMX233 || CONFIG_CPU == DM320 \
   || CONFIG_CPU == AS3525 || CONFIG_CPU == AS3525v2 \
   || CONFIG_CPU == S3C2440 || CONFIG_CPU == TCC7801 \
   || CONFIG_CPU == SC6530C \
   || defined(CPU_S5L87XX)
target/arm/mmu-arm.S
#  endif
```

### 5b. The `#ifdef B310E` model block

Insert a model block (mirror the GIGABEAT_F block at L1251-1264 or the
ECHO_R1 block at L1975-1991) — place it right after the GIGABEAT_F block:

```c
#ifdef B310E
drivers/lcd-memframe.c
target/arm/sc6530c/system-sc6530c.c
target/arm/sc6530c/kernel-sc6530c.c
target/arm/sc6530c/lcd-sc6530c.c
target/arm/sc6530c/button-sc6530c.c
target/arm/sc6530c/backlight-sc6530c.c
target/arm/sc6530c/powermgmt-sc6530c.c
target/arm/sc6530c/power-sc6530c.c
target/arm/sc6530c/sd-sc6530c.c
#ifndef BOOTLOADER
target/arm/sc6530c/pcm-sc6530c.c
target/arm/sc6530c/audiohw-sc6530c.c
#endif /* !BOOTLOADER */
#endif /* B310E */
```

Notes:
- `drivers/lcd-memframe.c` provides `lcd_update`/`lcd_update_rect` (copies
  Rockbox's framebuffer to FRAME via our `lcd_copy_buffer_rect`, which also
  triggers the LCDC DMA refresh).
- `drivers/lcd-16bit.c` is pulled in automatically by the existing
  `#if LCD_DEPTH == 16` block (SOURCES L404-409) — no edit needed there.
- `sd-sc6530c.c` is listed because `CONFIG_STORAGE STORAGE_SD` makes
  `storage_init()` reference `sd_*` (storage.h maps them); the link needs
  the symbols. It is a thin wrapper over the B310E-OS SDIO driver.
- pcm/audiohw are bootloader-unsafe (`#ifndef BOOTLOADER`), matching the
  per-model convention.

---

## 6. apps/SOURCES — keymap line

The keymap `#elif` chain runs L187-309. Add the branch before the closing
`#endif` on L309 (after `HIDIZS_AP80MAX_PAD` on L307-308):

```c
#elif CONFIG_KEYPAD == B310E_PAD
keymaps/keymap-b310e.c
```

There is NO `apps/keymaps/SOURCES` file (verified) — apps/SOURCES alone
selects keymaps.

---

## 7. Build recipe (for T2.2, not run here)

From the staged clone, in MSYS2 bash (`C:\msys64\usr\bin\bash.exe`):

```bash
tools/configure --target=b310e --type=N --compiler-prefix=arm-none-eabi-
make -j8
# -> rockbox.bin (raw, tool="cp") — copy to sdcard/progs/rockbox.bin
```

Configure is POSIX sh; it must run under MSYS2/WSL bash (not cmd/PowerShell).

---

## 8. Known build risks (documented, not fixed in this overlay)

1. **`HW_SAMPR_CAPS` needs 44 kHz alongside 8 kHz** — `pcm_sampr.h` L238-246
   `#error`s "Neither 48 or 44KHz supported?" if neither SAMPR_CAP_44 nor
   SAMPR_CAP_48 is set. config/b310e.h defines `(SAMPR_CAP_8 |
   SAMPR_CAP_44)` with a comment; the sink's own caps table stays {8000}.
   The DSP will resample everything to 8 kHz (correct for the VBC path).
2. **`MEMORYSIZE`** comes from configure (`memory=4`), NOT config.h.
3. **`AUDIOHW_SETTING`** is #defined empty by config.h L1479 outside
   sound.c — the codec header's call is a no-op in normal TUs, and sound.c
   (with `AUDIOHW_IS_SOUND_C`) generates the settings struct. Do NOT add
   another definition.
4. **`udelay()` is target-provided** (system-sc6530c.c) — no SoC timer
   dependency (see the file's comment).
5. **crt0.S enables the identity MMU** (required: the menu left CP15 V=1,
   so the tick IRQ needs 0xffff0000 mapped to the vector table). The
   descriptor values are the B310E-OS/fpdoom HW-proven patterns — do not
   "fix" them from the ARM ARM.
6. **SD storage bounces through IRAM** (0x4000b000) and masks the tick
   IRQ around the probe — B310E-OS hardware lessons; do not "simplify"
   to DMA-into-PSRAM.
7. **No 0x8c000000 pinmux writes anywhere** (they hang the phone); the
   only 0x8c writes in the whole tree are the SDIO pinmux regs inside the
   B310E-OS drivers/sdio.c, which sd-sc6530c.c calls indirectly.

---

## 9. Build-pipeline additions (discovered while making the first build)

These are applied by `build.sh` (marker-guarded, idempotent) IN ADDITION to
sections 1-7. Every one was found by actually building; each has a
`B310E-PORT` marker in the clone or a distinctive grep guard.

### 9.1 toolchain + host tools (environment, not file patches)

- **Space-free toolchain junction**: the arm-none-eabi toolchain lives
  under `C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1`
  — spaces AND parens break configure's arch detection (`-c line: /c/Program:
  No such file`) and make's CC line. build.ps1 creates a junction
  `C:\arm-gcc` → the real dir (build.sh uses `$TOOLCHAIN=/c/arm-gcc/bin`).
- **Host `gcc`**: MSYS2 has none; Rockbox host tools (rdf2binary, convbdf,
  bmp2rb, …) need it. build.sh adds the WinLibs mingw gcc (the one the
  B310E-OS host tests already use) to PATH. Override with `B310E_HOST_CC`.
- **`zip`**: MSYS2 base lacks it; the lang/voice pipeline builds
  `voicestrings.zip`. build.sh bootstraps `pacman -S --noconfirm zip`.
- **`output="rockbox.b310e"` (NOT "rockbox.bin")**: `output="rockbox.bin"`
  collides with Rockbox's internal objcopy intermediate (`rockbox.bin :
  rockbox.elf`) — the `$(BINARY)` rule becomes self-referential, the
  circular dependency is dropped, and the "bin" ends up being the ELF
  (silent: the build succeeds). Any distinct name works; build.sh copies
  `rockbox.b310e` → `sdcard/progs/rockbox.bin`.

### 9.2 tools/addtargetdir.pl — Windows-drive-path normalization

gcc.exe on MSYS emits dep paths as `C:/Users/…` while `$rootdir` is
`/c/Users/…`; the perl's length-based rootdir→builddir surgery then mangles
every make.dep target, so the parallel build gets NO header ordering and
races on generated headers (lang.h, sysfont.h, …). One line normalizes
before the surgery (insert after the `for (split(...))` line):

```perl
    s{^([A-Za-z]):}{"/" . lc($1)}e;
```

### 9.3 firmware/SOURCES — mmu-arm.S list gains SC6530C

`mmu-arm.S` provides `commit_dcache`/`commit_discard_*` (lcd-sc6530c.c
calls them) but its CONFIG_CPU list (L671-675) is explicit. Add
`|| CONFIG_CPU == SC6530C \` to the list. It only needs TTB_BASE_ADDR +
CACHEALIGN_BITS (both in sc6530c.h).

### 9.4 firmware/target/arm/mmu-arm.S — SC6530C cache geometry

Without a case, mmu-arm.S #errors "Cache settings unknown for this CPU!".
The ARM926EJ-S has the same 16 KB 4-way/32-byte-line D-cache as the
S3C2440's ARM920T. Add before the `#else`:

```c
#elif CONFIG_CPU == SC6530C
#define USE_MMU
#define CACHE_SIZE 16
```

### 9.5 apps/plugins/plugin.lds — SC6530C DRAM map

The codec/plugin link scripts #error "DRAM/IRAM memory map not defined!"
without a per-CPU DRAMORIG/DRAMSIZE. Mirror the app.lds budget (4 MB PSRAM
minus plugin/codec/LCD/TTB):

```c
#elif CONFIG_CPU == SC6530C
#define DRAMORIG 0x04000000
#define DRAMSIZE ((MEMORYSIZE * 0x100000) - PLUGIN_BUFFER_SIZE \
                  - CODEC_SIZE - LCD_BUFFER_SIZE - TTB_SIZE)
```

### 9.6 tools/functions.make — asmdefs extractor must strip CR

gcc.exe (native Windows) writes CRLF assembly, so every line ends `\r`,
the `AD_<name>`/`.word` regexes never match and every `*_asmdefs.h` comes
out EMPTY — then every .S file using the exported symbols fails to
assemble (jpeg_idct_arm.S: "invalid operands (*ABS* and *UND* sections)").
Note the `$` must NOT appear in the makefile text (make eats `$`): use
`s/\r//g`:

```make
perl -ne 's/\r//g;if(/^_?AD_(\w+):$$/){...}'
```

### 9.7 tools/genlang — relative lang_enum.h include

genlang bakes `#include "${prefix}_enum.h"` into the generated lang.h. On
MSYS the prefix is `/c/...`, which the native arm-none-eabi-gcc
preprocessor cannot open — every TU including lang.h fails on lang_enum.h.
Emit a relative include (lang.h is in `<build>/lang`, lang_enum.h in
`<build>`):

```c
#include "../lang_enum.h"
```

### 9.8 firmware/SOURCES — crt0.S per-CPU entry

Without it SC6530C falls through to the generic `target/arm/crt0.S`, so
crt0.o is never built and app.lds's `STARTUP(...crt0.o)` fails at link:

```c
#elif CONFIG_CPU==SC6530C
target/arm/sc6530c/crt0.S
#elif CONFIG_CPU==RK27XX
```

### 9.9 Generated-header ordering (build.sh, not a patch)

A truly-fresh -j8 build races app .o files against the GENERATED headers
(lang.h → lang_enum.h chain, sysfont.h, bitmap headers): the -MG dep scan
cannot see through a not-yet-existing header, so make.dep has no edge for
the second-level files. build.sh runs a serial pre-pass
(`make -j1 lang/lang_core.o sysfont.h rbversion.h bitmaps/rockboxlogo.h
lang_enum.h fontbundle.h`) then `make -j8` (up to 3 passes).

### 9.10 Overlay target files added during bring-up

- `adc-target.h` + `adc-sc6530c.c` — Rockbox's adc.h needs the channel
  enum + `adc_read`/`adc_init` (apps/debug_menu.c links them). The B310E
  battery is read by the SC6530 internal ADC directly in
  powermgmt-sc6530c.c; adc_read returns 0.
- `rtc-sc6530c.c` — CONFIG_RTC RTC_SC6530 makes the generic timefuncs call
  `rtc_read_datetime`/`rtc_write_datetime`; port of the B310E-OS on-die RTC.
- `timer-sc6530c.c` — Rockbox's user-timer API (timer_set/start/stop) on
  SC6530 TIMER 0 (IRQ line 4); TIMER 2 (line 23) stays the tick.
- `i2c-sc6530c.c` — apps/main.c calls `i2c_init()` unconditionally; the
  SC6530 has no I2C peripherals here (CONFIG_I2C I2C_NONE).
- `sdio-sc6530c.c` — the REAL SC6530 SDIO driver: a port of the B310E-OS
  drivers/sdio.c (fpdoom SC6530 branch, Unlicense, HW-validated) adapted to
  Rockbox (REG32/logf/commit_discard_dcache/disable_irq), + a standard CSD
  parse exposing `sdio_num_blocks` for the storage geometry (M2 replaced
  the M1 no-card stub).
- `dbg_ports()`/`dbg_hw_info()` (system-sc6530c.c) + `fiq_handler`
  (crt0.S) + the line-4 IRQ dispatch — all found by the linker.
- sd-sc6530c.c: the GENERIC firmware/drivers/sd.c provides sd_get_info
  (via `card_get_info_target`), sd_spin and sd_spindown — the target file
  must NOT redefine them (duplicate-symbol errors).
