# B310E-OS Makefile — Spreadtrum SC6530C (ARM926EJ-S, ARMv5TE)
# Cross toolchain:  Arm GNU Toolchain 14.2.Rel1 (winget: Arm.GnuArmEmbeddedToolchain)
# Host toolchain:   MinGW-W64 gcc 16.1.0 (winget: BrechtSanders.WinLibs.POSIX.UCRT)
# Make:             GNU Make 4.4.1 (winget: ezwinports.make) — run plain `make`,
#                   NOT mingw32-make.
#
# PATH note: winget installs these WITHOUT adding them to PATH. Detection
# below prefers tools already on PATH and falls back to pinned install dirs.

# Force cmd.exe as the make shell on Windows. Without this, GNU make picks
# whatever `sh` it finds on PATH first - if git-bash's sh is present, the
# `2>NUL` probes below (and any >NUL recipe redirect) create a LITERAL file
# named NUL instead of redirecting to the device. cmd.exe keeps `2>NUL` a
# device redirect. The recipes (findstr, >NUL, powershell) already assume cmd.
# Use the ABSOLUTE path: a bare "cmd.exe" makes make do a PATH lookup, and if
# that fails Windows pops a "Select an app to open 'cmd'" dialog instead of
# erroring. cmd.exe is always at this path on every Windows system.
# On Linux make's default shell (/bin/sh) is used; only the probes differ.
ifeq ($(OS),Windows_NT)
SHELL := C:\Windows\System32\cmd.exe
endif

CROSS := arm-none-eabi-

# ---- cross toolchain detection -------------------------------------------
ifeq ($(OS),Windows_NT)
# Explicitly run the probe under cmd (/c) so it never depends on which shell
# make's $(shell) picked: under git-bash sh, `2>NUL` creates a literal file
# named NUL, and bare `where`/`cmd` can trigger Windows' "pick an app" dialog.
GCC := $(shell cmd /c "where $(CROSS)gcc 2>NUL")
ifeq ($(GCC),)
GCC := $(shell cmd /c "where $(CROSS)gcc")
endif
ifeq ($(GCC),)
export PATH := C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin;$(PATH)
$(info B310E-OS: $(CROSS)gcc not on PATH; using pinned toolchain dir fallback)
endif

# ---- host toolchain detection (hosttest) ---------------------------------
HOSTCC := $(shell cmd /c "where gcc 2>NUL")
ifeq ($(HOSTCC),)
HOSTCC := $(shell cmd /c "where gcc")
endif
ifeq ($(HOSTCC),)
# Find the WinLibs gcc under the current user's WinGet package cache
# (LOCALAPPDATA is the standard winget location - no username hardcoded).
HOSTCC := $(shell powershell -NoProfile -Command "$$p = Get-ChildItem '$$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT*\mingw64\bin\gcc.exe' -ErrorAction SilentlyContinue | Select-Object -First 1; if ($$p) { $$p.FullName }")
$(info B310E-OS: host gcc not on PATH; using WinLibs gcc fallback from LOCALAPPDATA)
endif
else
# ---- Linux toolchain detection (command -v; no winget fallback) ----------
GCC := $(shell command -v $(CROSS)gcc 2>/dev/null)
ifeq ($(GCC),)
$(info B310E-OS: $(CROSS)gcc not found on PATH - install gcc-arm-none-eabi (apt/pacman/dnf) or add its bin/ to PATH)
endif
HOSTCC := $(shell command -v gcc 2>/dev/null)
ifeq ($(HOSTCC),)
$(info B310E-OS: host gcc not found on PATH - install gcc (apt/pacman/dnf))
endif
endif

# ---- platform command shims --------------------------------------------------
# Windows recipes shell out to PowerShell (UNC-safe: cmd's `del` breaks on a
# UNC cwd); Linux uses the standard coreutils. Call from recipes with
# $(call NAME,arg1,arg2,...). HOSTTEST_EXE / RUN_* are full command lines.
ifeq ($(OS),Windows_NT)
MKDIR_CMD  = powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '$(1)' | Out-Null; exit 0"
TOUCH_CMD  = powershell -NoProfile -Command "New-Item -ItemType File -Force -Path '$(1)' | Out-Null; exit 0"
COPY_CMD   = powershell -NoProfile -Command "Copy-Item -Force '$(1)' '$(2)'; exit 0"
CAT_APPEND = powershell -NoProfile -Command "$$r=[System.IO.File]::ReadAllBytes('$(1)'); $$f=[System.IO.File]::Open('$(2)',[System.IO.FileMode]::Append); $$f.Write($$r,0,$$r.Length); $$f.Close(); exit 0"
RM_RECURSIVE = powershell -NoProfile -Command "Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -Path '$(1)'; exit 0"
# `make clean` also removes the GENERATED parts of sdcard/ (fpbin, progs,
# .rockbox, games/README.md + empty game dirs) so a fresh `make sdcard`
# rebuilds everything - but PRESERVES sdcard/games/ because the user copies
# their game data files (WADs/ROMs/GRPs) into it per games/README.md.
RM_SDCARD_GENERATED = powershell -NoProfile -Command "if (Test-Path '$(CURDIR)/sdcard') { Get-ChildItem '$(CURDIR)/sdcard' | Where-Object { $$_.Name -ne 'games' } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue }; exit 0"
# echo shim: cmd prints parens raw, so no quoting on Windows; /bin/sh would
# parse "(...)" as a subshell, so the message is double-quoted on Linux.
MSG = echo $(1)
HOSTTEST_EXE := hosttest.exe
CHECK_ENTRY = $(READELF) -h $(CURDIR)\$(TARGET).elf | findstr /C:"0x14000010" >NUL && echo check: entry _start @ 0x14000010 PASS
CHECK_TEXT  = $(READELF) -S $(CURDIR)\$(TARGET).elf | findstr /C:"14000000" >NUL && echo check: .text @ 0x14000000 PASS
RUN_FPMAIN  = powershell -NoProfile -ExecutionPolicy Bypass -File $(CURDIR)\tools\fpmain\build-fpmain.ps1
RUN_GAMES   = powershell -NoProfile -ExecutionPolicy Bypass -File $(CURDIR)\tools\fpmain\rebuild-games.ps1
RUN_ROCKBOX = powershell -NoProfile -ExecutionPolicy Bypass -File $(CURDIR)\tools\rockbox-port\build.ps1
RUN_STOCKRAM      = powershell -NoProfile -ExecutionPolicy Bypass -File $(CURDIR)\tools\stockram\pack-stockram.ps1
RUN_STOCKRAM_DIAG = powershell -NoProfile -ExecutionPolicy Bypass -File $(CURDIR)\tools\stockram\diag-pack.ps1
else
MKDIR_CMD  = mkdir -p '$(1)'
TOUCH_CMD  = touch '$(1)'
COPY_CMD   = cp -f '$(1)' '$(2)'
CAT_APPEND = cat '$(1)' >> '$(2)'
RM_RECURSIVE = rm -rf '$(1)'
# Preserve sdcard/games/ (user-copied game data); remove the generated rest.
RM_SDCARD_GENERATED = if [ -d '$(CURDIR)/sdcard' ]; then find '$(CURDIR)/sdcard' -mindepth 1 -maxdepth 1 ! -name games -exec rm -rf {} +; fi
MSG = echo "$(1)"
HOSTTEST_EXE := hosttest
CHECK_ENTRY = $(READELF) -h $(CURDIR)/$(TARGET).elf | grep -q 0x14000010 && echo check: entry _start @ 0x14000010 PASS
CHECK_TEXT  = $(READELF) -S $(CURDIR)/$(TARGET).elf | grep -q 14000000 && echo check: .text @ 0x14000000 PASS
RUN_FPMAIN  = bash $(CURDIR)/tools/fpmain/build-fpmain.sh
RUN_GAMES   = bash $(CURDIR)/tools/fpmain/rebuild-games.sh
RUN_ROCKBOX = bash $(CURDIR)/tools/rockbox-port/build.sh
RUN_STOCKRAM      = bash $(CURDIR)/tools/stockram/pack-stockram.sh
RUN_STOCKRAM_DIAG = bash $(CURDIR)/tools/stockram/diag-pack.sh
endif

CC      := $(CROSS)gcc
LD      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
READELF := $(CROSS)readelf
SIZE    := $(CROSS)size

ARCH    := armv5te

CFLAGS  := -Os -Wall -Wextra -funsigned-char -fno-PIE -ffreestanding \
           -march=$(ARCH) -mthumb -fomit-frame-pointer \
           -ffunction-sections -fdata-sections -std=c11 -pedantic \
           -Iinclude -Ikernel -Ibuild
ASFLAGS := -march=$(ARCH)     # start.s is ARM state; ctx.s selects Thumb itself
# -pie: the image ships a pack_reloc .rel table; arch/start.s applies it
# (diff = runtime - link) so os.bin runs at 0x14000000 (sdboot readbin) or
# 0x34000000 (spd_dump ram). diag images run at their link base (diff 0).
LDFLAGS := -pie -nostartfiles -nodefaultlibs -nostdlib \
           -Wl,-T,link/os.ld -Wl,--gc-sections -Wl,-z,notext

# pack_reloc (host tool): extracts/compresses the ELF relocations into the
# .rel table appended to os.bin / os-sd.bin. Vendored in tools/pack_reloc/
# (mingw lacks elf.h — the musl copy sits in tools/pack_reloc/inc/; Linux
# builds use the same vendored header for consistency).
ifeq ($(OS),Windows_NT)
PACK_RELOC := tools/pack_reloc/pack_reloc.exe
else
PACK_RELOC := tools/pack_reloc/pack_reloc
endif

$(PACK_RELOC): tools/pack_reloc/pack_reloc.c
	$(HOSTCC) -O2 -Wall -Wextra -std=c99 -pedantic -Wno-unused \
	    -Itools/pack_reloc/inc -o $@ $<

# ---- build-time stamp (QoL: the RTC is bootstrapped to it if behind) ------
# Generated on every make into build/build_time.h (gitignored): the compile
# date/time, used by app/demo.c to set a battery-removed RTC.
BUILD_TIME_H := build/build_time.h

# Bare `make` must build os.bin, NOT the first rule below (the object files'
# build_time.h dependency, which would silently no-op when they are fresh).
.DEFAULT_GOAL := all

# every object depends on the generated stamp so make builds it BEFORE
# compiling (app/demo.o #includes it); without this, make compiles the .o
# files before the elf's build_time.h prerequisite is generated.
# NOTE: this rule MUST live AFTER the OBJS assignment below (immediate :=
# expansion) — placed earlier it expands to an empty target and the
# dependency silently vanishes, breaking `make` from a clean tree.
$(BUILD_TIME_H):
ifeq ($(OS),Windows_NT)
	@powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '$(CURDIR)/build' | Out-Null; $$y=[int](Get-Date -Format yyyy); $$mo=[int](Get-Date -Format MM); $$d=[int](Get-Date -Format dd); $$h=[int](Get-Date -Format HH); $$mi=[int](Get-Date -Format mm); $$s=[int](Get-Date -Format ss); Set-Content -NoNewline -Path '$(CURDIR)/build/build_time.h' -Value (\"#define BUILD_TIME_YEAR $$y`n#define BUILD_TIME_MONTH $$mo`n#define BUILD_TIME_DAY $$d`n#define BUILD_TIME_HOUR $$h`n#define BUILD_TIME_MIN $$mi`n#define BUILD_TIME_SEC $$s`n\")"
else
	@mkdir -p '$(CURDIR)/build'
	@printf '#define BUILD_TIME_YEAR %s\n' "$$(date +%Y)" > '$(CURDIR)/build/build_time.h'
	@printf '#define BUILD_TIME_MONTH %s\n' "$$(date +%-m)" >> '$(CURDIR)/build/build_time.h'
	@printf '#define BUILD_TIME_DAY %s\n' "$$(date +%-d)" >> '$(CURDIR)/build/build_time.h'
	@printf '#define BUILD_TIME_HOUR %s\n' "$$(date +%-H)" >> '$(CURDIR)/build/build_time.h'
	@printf '#define BUILD_TIME_MIN %s\n' "$$(date +%-M)" >> '$(CURDIR)/build/build_time.h'
	@printf '#define BUILD_TIME_SEC %s\n' "$$(date +%-S)" >> '$(CURDIR)/build/build_time.h'
endif

# ---- objects --------------------------------------------------------------
# kernel/*.c, drivers/*.c and app/*.c auto-add to the ARM link. The host
# test build is separate (HOSTTEST_SRCS): kernel + tests only — app/driver
# code is ARM-only and would not compile for x86.
# BINDIR: every firmware image (os.bin, os-sd.bin, os-diag-*.bin, os-dsp-boot.bin
# + their .elf/.rel) builds here, keeping the repo root source-only. The
# *_TARGET vars below are full paths into BINDIR so every $(TARGET).xxx rule
# resolves to build/bin/<name>.xxx. build/ is gitignored + removed by make clean.
BINDIR := build/bin
TARGET  := $(BINDIR)/os
KERNEL_OBJS := $(patsubst %.c,%.o,$(wildcard kernel/*.c))
DRIVER_OBJS := $(patsubst %.c,%.o,$(wildcard drivers/*.c))
APP_OBJS    := $(patsubst %.c,%.o,$(wildcard app/*.c))
OBJS    := arch/start.o arch/smc_init.o arch/main.o arch/ctx.o arch/cache.o arch/vectors.o arch/mmu.o $(KERNEL_OBJS) $(DRIVER_OBJS) $(APP_OBJS)

# ensure BINDIR exists before any link writes into it (created once, then
# kept - make clean removes the whole build/ tree)
$(BINDIR):
	@$(call MKDIR_CMD,$(CURDIR)/$(BINDIR))

# ordering dependency: every object that #includes build_time.h depends on
# the generated stamp, so make builds the header BEFORE compiling the .o.
# (Must come after the OBJS := above — see the comment at BUILD_TIME_H.)
$(OBJS): $(BUILD_TIME_H)

# Diagnostic boot images (built together by `make debug` -> os-diag-*.bin):
# same kernel + drivers, but arch/diag_lcd_main.o / arch/diag_rot_main.o
# REPLACE arch/main.o. They call usb_debug_init/lcd_init directly — no
# module framework, no scheduler. (The old diag-s0..s5 chip-init stage
# bisect was retired once the boot hang was root-caused and all stages
# were verified on hardware.)
DIAG_OBJS     := arch/start.o arch/smc_init.o arch/ctx.o arch/cache.o arch/vectors.o arch/mmu.o $(KERNEL_OBJS) $(DRIVER_OBJS)

# Recursive (=): the *_TARGET vars are defined LATER in this file (diag/os-sd
# sections), so immediate (:=) expansion here would leave them empty and
# `make clean` would silently miss those artifacts.
CLEAN_FILES = $(OBJS) arch/diag_lcd_main.o arch/diag_rot_main.o arch/diag_sd_main.o arch/diag_sd_mmu_main.o arch/diag_dsp_main.o arch/diag_nor_main.o arch/menu_boot.o arch/menu_boot_nokey.o arch/menu_copy.o $(TARGET).elf $(TARGET).bin $(TARGET).rel $(OS_SD_TARGET).elf $(OS_SD_TARGET).bin $(OS_SD_TARGET).rel drivers/usb_debug_sd.o $(PACK_RELOC) $(DIAG_LCD_TARGET).elf $(DIAG_LCD_TARGET).bin $(DIAG_ROT_TARGET).elf $(DIAG_ROT_TARGET).bin $(DIAG_SD_TARGET).elf $(DIAG_SD_TARGET).bin $(DIAG_SDMMU_TARGET).elf $(DIAG_SDMMU_TARGET).bin $(DIAG_NOR_TARGET).elf $(DIAG_NOR_TARGET).bin build/$(HOSTTEST_EXE) build/build_time.h tools/fpmain/.games-stamp

# NOTE: `del` (cmd builtin) breaks when make runs from a UNC path (cmd
# defaults to C:\Windows); PowerShell handles the UNC cwd natively.
# (PS 5.1 quirk: Remove-Item needs -Path a,b (comma list), not bare positionals.)
# Recursive (=) too: re-expands $(CLEAN_FILES) at recipe time, so the later
# `CLEAN_FILES +=` (dsp-boot) is included.
empty :=
space := $(empty) $(empty)
comma := ,
ifeq ($(OS),Windows_NT)
CLEAN = powershell -NoProfile -Command "Remove-Item -Force -ErrorAction SilentlyContinue -Path $(subst $(space),$(comma),$(CLEAN_FILES)); exit 0"
else
CLEAN = rm -f $(CLEAN_FILES)
endif

.PHONY: all dis size clean hosttest check os-sd debug

all: $(TARGET).bin

$(TARGET).elf: $(OBJS) $(BUILD_TIME_H) link/os.ld | $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(TARGET).rel: $(TARGET).elf $(PACK_RELOC)
	$(PACK_RELOC) $< $@

$(TARGET).bin: $(TARGET).elf $(TARGET).rel
	$(OBJCOPY) -O binary -j .text -j .rodata -j .data $< $@
	@$(call CAT_APPEND,$(TARGET).rel,$(TARGET).bin)
	@$(call MSG,os.bin: appended .rel (relocatable - runs at 0x14000000 readbin or 0x34000000 ram))

# ---- SD-boot OS variant (os-sd.bin) -----------------------------------------
# `make os-sd` -> os-sd.bin: os.bin with the USB debug module init SKIPPED
# (-DSD_BOOT_NO_USB). On a card boot the USB block is UNPOWERED — a register
# write to it stalls the AHB bus and hangs the phone, and the modules init in
# registration order with usb_debug FIRST. The card's progs/os.bin must be
# this variant (staged by `make sdcard`); the repo-root os.bin stays the
# USB/libc_server build.
OS_SD_TARGET := $(BINDIR)/os-sd

.PHONY: os-sd

os-sd: $(OS_SD_TARGET).bin

drivers/usb_debug_sd.o: drivers/usb_debug.c $(BUILD_TIME_H)
	$(CC) $(CFLAGS) -DSD_BOOT_NO_USB -c -o $@ $<

$(OS_SD_TARGET).elf: $(filter-out drivers/usb_debug.o,$(OBJS)) drivers/usb_debug_sd.o link/os.ld | $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ $(filter-out drivers/usb_debug.o,$(OBJS)) drivers/usb_debug_sd.o

$(OS_SD_TARGET).rel: $(OS_SD_TARGET).elf $(PACK_RELOC)
	$(PACK_RELOC) $< $@

$(OS_SD_TARGET).bin: $(OS_SD_TARGET).elf $(OS_SD_TARGET).rel
	$(OBJCOPY) -O binary -j .text -j .rodata -j .data $< $@
	@$(call CAT_APPEND,$(OS_SD_TARGET).rel,$(OS_SD_TARGET).bin)
	@$(call MSG,os-sd.bin: appended .rel (relocatable))

# ---- LCD diagnostic image ---------------------------------------------------
# part of `make debug` (all diag images): same kernel + drivers as os-diag, but
# arch/diag_lcd_main.c REPLACES BOTH arch/main.o and arch/diag_main.o. Runs
# usb_debug_init -> lcd_init -> staged RED/GREEN/BLUE sequence with BOUNDED
# refresh waits (lcd_show_bounded from drivers/lcd.c) -> halt. Discriminates
# why the panel shows colorful static instead of the banner. No chip init
# (matches os-diag-s0's proven boot path), no scheduler.
DIAG_LCD_TARGET := $(BINDIR)/os-diag-lcd
DIAG_LCD_OBJS   := arch/start.o arch/smc_init.o arch/ctx.o arch/cache.o arch/vectors.o arch/mmu.o $(KERNEL_OBJS) $(DRIVER_OBJS)

$(DIAG_LCD_TARGET).elf: arch/diag_lcd_main.o $(DIAG_LCD_OBJS) link/menu.ld | $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ arch/diag_lcd_main.o $(DIAG_LCD_OBJS)

$(DIAG_LCD_TARGET).bin: $(DIAG_LCD_TARGET).elf
	$(OBJCOPY) -O binary -j .text -j .rodata -j .data $< $@

# ---- rotation diagnostic image ----------------------------------------------
# part of `make debug`: same link set as diag-lcd, but
# arch/diag_rot_main.c cycles candidate ST7735 MADCTL values on the live
# panel (via the lcd_set_madctl helper in drivers/lcd.c) so the user can
# pick the upright screen orientation in one run. No chip init, no scheduler.
DIAG_ROT_TARGET := $(BINDIR)/os-diag-rot
DIAG_ROT_OBJS   := $(DIAG_LCD_OBJS)

$(DIAG_ROT_TARGET).elf: arch/diag_rot_main.o $(DIAG_ROT_OBJS) link/menu.ld | $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ arch/diag_rot_main.o $(DIAG_ROT_OBJS)

$(DIAG_ROT_TARGET).bin: $(DIAG_ROT_TARGET).elf
	$(OBJCOPY) -O binary -j .text -j .rodata -j .data $< $@

# ---- SD diagnostic image ---------------------------------------------------
# part of `make debug`: same link set as diag-lcd, but
# arch/diag_sd_main.c runs FULL chip init + LCD banner + sdio_init +
# sdcard_init + reads the MBR sector. First image to touch the SDIO
# controller and its pinmux regs 0x8c000250-0x264 — validates the SD
# stack on hardware before Rockbox depends on it.
DIAG_SD_TARGET := $(BINDIR)/os-diag-sd
DIAG_SD_OBJS   := $(DIAG_LCD_OBJS)

$(DIAG_SD_TARGET).elf: arch/diag_sd_main.o $(DIAG_SD_OBJS) link/menu.ld | $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ arch/diag_sd_main.o $(DIAG_SD_OBJS)

$(DIAG_SD_TARGET).bin: $(DIAG_SD_TARGET).elf
	$(OBJCOPY) -O binary -j .text -j .rodata -j .data $< $@

# ---- SD diagnostic with the MMU ON (environment bisect) --------------------
# part of `make debug`: replicates os.bin's EXACT
# pre-probe init (chip init -> enable_dcache -> modules -> irq_init/MMU)
# but runs the SD probe directly in main() — no scheduler, no tick.
# Bisects the stage-5 SD death: works here => scheduler context is the
# killer; dies at stage 5 => MMU/D-cache/prior state is the killer.
DIAG_SDMMU_TARGET := $(BINDIR)/os-diag-sdmmu
DIAG_SDMMU_OBJS   := $(DIAG_SD_OBJS)

$(DIAG_SDMMU_TARGET).elf: arch/diag_sd_mmu_main.o $(DIAG_SDMMU_OBJS) link/menu.ld | $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ arch/diag_sd_mmu_main.o $(DIAG_SDMMU_OBJS)

$(DIAG_SDMMU_TARGET).bin: $(DIAG_SDMMU_TARGET).elf
	$(OBJCOPY) -O binary -j .text -j .rodata -j .data $< $@

# ---- DSP boot diagnostic image --------------------------------------------
# part of `make debug`: boots the SC6530C's TeakLite DSP on
# hardware and gives a visible verdict (LCD + keylight ladder). Embeds the
# decompressed DSP program (seg0 of the 5-segment bundle) extracted at build
# time from the gitignored .omo/dumpmine/stock-fw/dsp-blob-CC874.dec.bin
# (proprietary stock data stays OUT of the repo — the rule errors loudly if
# the artifact is missing). Relocatable like os.bin (os.ld + -pie + .rel):
# runs at 0x04000000 via the fpmain readbin launch (progs/, .rel diff
# -0x10000000) or self-relocates to 0x34000000 via spd_dump. Compiled with
# -DSD_BOOT_NO_USB like os-sd.bin: the card-boot launch has the USB block
# UNPOWERED (a register write would stall the AHB and hang the phone).
DSPBOOT_TARGET := $(BINDIR)/os-dsp-boot
DSPBOOT_OBJS   := $(DIAG_LCD_OBJS)

build/dsp_seg0.bin: .omo/dumpmine/stock-fw/dsp-blob-CC874.dec.bin
ifeq ($(OS),Windows_NT)
	@powershell -NoProfile -Command "$$f=[System.IO.File]::ReadAllBytes('$(CURDIR)/.omo/dumpmine/stock-fw/dsp-blob-CC874.dec.bin'); $$h=(Get-FileHash '$(CURDIR)/.omo/dumpmine/stock-fw/dsp-blob-CC874.dec.bin' -Algorithm SHA256).Hash; if ($$h -ne 'BC64236A0613D9838755BDC8E819811D07AAD9C29742F55F9989E422D8EE5604') { Write-Error 'dsp-blob-CC874.dec.bin changed - re-verify the seg0 offsets'; exit 1 }; $$s=New-Object byte[] 0x20A5A; [Array]::Copy($$f,0x28,$$s,0,0x20A5A); New-Item -ItemType Directory -Force -Path '$(CURDIR)/build' | Out-Null; [System.IO.File]::WriteAllBytes('$(CURDIR)/build/dsp_seg0.bin',$$s); exit 0"
else
	@h=$$(sha256sum '$(CURDIR)/.omo/dumpmine/stock-fw/dsp-blob-CC874.dec.bin' | awk '{print toupper($$1)}'); if [ "$$h" != "BC64236A0613D9838755BDC8E819811D07AAD9C29742F55F9989E422D8EE5604" ]; then echo 'dsp-blob-CC874.dec.bin changed - re-verify the seg0 offsets' >&2; exit 1; fi; mkdir -p '$(CURDIR)/build'; dd if='$(CURDIR)/.omo/dumpmine/stock-fw/dsp-blob-CC874.dec.bin' of='$(CURDIR)/build/dsp_seg0.bin' bs=1 skip=40 count=133722 status=none
endif

build/dsp_seg0.o: build/dsp_seg0.bin
	$(OBJCOPY) -I binary -O elf32-littlearm -B arm $< $@

arch/diag_dsp_main.o: arch/diag_dsp_main.c
	$(CC) $(CFLAGS) -DSD_BOOT_NO_USB -c -o $@ $<

$(DSPBOOT_TARGET).elf: arch/diag_dsp_main.o build/dsp_seg0.o $(DSPBOOT_OBJS) link/os.ld | $(BINDIR)
	$(LD) $(LDFLAGS) -o $@ arch/diag_dsp_main.o build/dsp_seg0.o $(DSPBOOT_OBJS)

$(DSPBOOT_TARGET).rel: $(DSPBOOT_TARGET).elf $(PACK_RELOC)
	$(PACK_RELOC) $< $@

$(DSPBOOT_TARGET).bin: $(DSPBOOT_TARGET).elf $(DSPBOOT_TARGET).rel
	$(OBJCOPY) -O binary -j .text -j .rodata -j .data $< $@
	@$(call CAT_APPEND,$(DSPBOOT_TARGET).rel,$(DSPBOOT_TARGET).bin)
	@$(call MSG,os-dsp-boot.bin: appended .rel (relocatable - fpmain 0x04000000 / spd_dump 0x34000000))

CLEAN_FILES += arch/diag_dsp_main.o build/dsp_seg0.o build/dsp_seg0.bin \
               $(DSPBOOT_TARGET).elf $(DSPBOOT_TARGET).bin $(DSPBOOT_TARGET).rel

%.o: %.s
	$(CC) $(ASFLAGS) -x assembler-with-cpp -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

dis: $(TARGET).elf
	$(OBJDUMP) -d $(TARGET).elf

size: $(TARGET).elf
	$(SIZE) $(TARGET).elf

# ---- RETIRED: menu.bin / fpmain.bin (the merged-boot SD-card menu) ----------
# `make menu` (menu.bin, the merged boot stub + key check + stock fallback +
# boot menu) and the OBSOLETE `make fpmain` (menu.bin built as the sdboot
# payload) were REMOVED 2026-08-28. The boot menu is now the PORTED fpdoom
# fpmain app (tools/fpmain/, built by tools/fpmain/build-fpmain.ps1, staged by
# `make sdcard` as fpbin/fpmain.bin) launched by the clean fpdoom sdboot3.bin
# NOR loader (docs/sdboot.md). Kept for the diag family below:
#   - link/menu.ld        - the fixed-base link script for all diag images
#   - arch/menu_boot.s    - compiled -DMENU_NO_KEYCHECK -> menu_boot_nokey.o
#                           (the PSRAM bring-up + copy + jump stub) for diag-nor
#   - arch/menu_copy.s    - ARM cache-flush + Thumb switch helpers (historical
#                           reference; no current target links menu_copy.o)
# MENU_LDFLAGS is the diag-family link line (menu.ld ALONE - appending -T to
# LDFLAGS concatenates both scripts and ld evaluates __image_size as 0, which
# breaks the header +8 field).
MENU_LDFLAGS := -pie -nostartfiles -nodefaultlibs -nostdlib \
                -Wl,-T,link/menu.ld -Wl,--gc-sections -Wl,-z,notext

# ---- NOR-boot diagnostic image -------------------------------------------
# part of `make debug`: the FIRST test of whether ANY of our
# code executes from the NOR boot vector at power-on (nothing had ever been
# NOR-booted before the 2026-08-23 menu install showed "doesn't power on").
# Uses the merged boot stub compiled with -DMENU_NO_KEYCHECK (no keypad
# probe, no stock chain - just PSRAM bring-up + copy + jump), then
# arch/diag_nor_main.c runs the PROVEN chip init and lights the keypad as a
# visible "we are alive" marker, then halts. Flash at 0x3fc000 + point the
# boot vector at it (same install as the retired menu.bin, docs/sdboot.md).
DIAG_NOR_TARGET := $(BINDIR)/os-diag-nor
# menu_boot_nokey.o = the merged boot stub WITHOUT the keypad probe /
# stock chain (-DMENU_NO_KEYCHECK): just PSRAM bring-up + copy + jump.
DIAG_NOR_OBJS   := arch/start.o arch/smc_init.o arch/ctx.o arch/cache.o \
                   arch/vectors.o arch/mmu.o arch/menu_boot_nokey.o \
                   $(KERNEL_OBJS) $(DRIVER_OBJS)

arch/menu_boot_nokey.o: arch/menu_boot.s
	$(CC) -x assembler-with-cpp $(ASFLAGS) -DMENU_NO_KEYCHECK -c -o $@ $<

$(DIAG_NOR_TARGET).elf: arch/diag_nor_main.o $(DIAG_NOR_OBJS) link/menu.ld | $(BINDIR)
	$(LD) $(MENU_LDFLAGS) -o $@ arch/diag_nor_main.o $(DIAG_NOR_OBJS)

$(DIAG_NOR_TARGET).bin: $(DIAG_NOR_TARGET).elf
	$(OBJCOPY) -O binary -j .text -j .rodata -j .data $< $@

# ---- ALL diagnostic images ------------------------------------------------
# `make debug` -> every diag image: os-diag-lcd.bin (LCD color stages),
# os-diag-rot.bin (MADCTL orientation cycler), os-diag-sd.bin (SDIO+FAT
# hardware validation), os-diag-sdmmu.bin (SD probe with MMU/D-cache ON),
# os-diag-nor.bin (NOR boot-vector test), os-dsp-boot.bin (TeakLite DSP
# boot verdict). The individual `make diag-*` aliases were removed 2026-08-28
# - these images are build-tooling for hardware bring-up, not user-facing
# targets, so they build only through this one gate.
.PHONY: debug

debug: $(DIAG_LCD_TARGET).bin $(DIAG_ROT_TARGET).bin $(DIAG_SD_TARGET).bin \
       $(DIAG_SDMMU_TARGET).bin $(DIAG_NOR_TARGET).bin $(DSPBOOT_TARGET).bin


# ---- SD-card boot (clean fpdoom sdboot3.bin as the NOR loader) -------------
# `make sdcard` -> stage the COMPLETE card. The NOR loader is the CLEAN
# fpdoom sdboot3.bin (tools/spd_dump/sdboot3.bin, 5076 B, CHIP=3 + B310E
# keypad fix, NOR-installed at 0x3fc000 per docs/sdboot.md): any key held ->
# full chip init -> reads fpbin/fpmain.bin -> copies it to PSRAM 0x04000000
# (NOR-boot window, MEM_REMAP=0) -> jumps ARM-state. So:
#   sdcard/fpbin/fpmain.bin   = the PORTED boot menu (tools/fpmain/fpmain.bin,
#       built by tools/fpmain/build-fpmain.ps1): the fpdoom fpmain app
#       skeleton with OUR progs/ launcher (B310E BOOT menu; launch flips
#       MEM_REMAP=1 + SMC re-init via the IRAM stub, copies to 0x34000000).
#   sdcard/fpbin/*.bin        = the fpdoom game ports (rebuild-games.ps1).
#   sdcard/progs/os.bin       = the OS the menu launches.
#   sdcard/progs/os-dsp-boot.bin = the DSP boot/audio test image (make debug).
#   sdcard/progs/rockbox.bin  = the Rockbox port (rockbox-port/build.ps1).
# Hold ANY key at power-on -> sdboot -> our menu -> pick os.bin. No key ->
# stock boot (sdboot header +4 = 0x46e4).
SD_CARD_DIR  := sdcard

# ---- fpdoom game ports (sdcard/fpbin/*.bin) --------------------------------
# `make games` -> rebuilds the 14 custom-patched fpdoom game ports via
# tools/fpmain/rebuild-games.ps1 (FAT_WRITE=0 read-only FAT so the phone can
# never corrupt the card; the gnuboy/snes9x save emulators get FAT_WRITE=1)
# and stages the .bins into sdcard/fpbin/. Gated on a stamp file so `make
# sdcard` only rebuilds them when the script or the PORTS menu configs change
# (the actual builds run in the fpdoom clone, not the repo toolchain). Prereq:
# the fpdoom clone at build/fpdoom (auto-cloned by the script; docs/sdboot.md);
# sources are re-downloaded by the script on first run (network).
GAMES_STAMP := tools/fpmain/.games-stamp

$(GAMES_STAMP): tools/fpmain/rebuild-games.ps1 tools/fpmain/rebuild-games.sh tools/fpmain/config.json tools/fpmain/config.txt
	$(RUN_GAMES)
	@$(call TOUCH_CMD,$(CURDIR)/$(GAMES_STAMP))

.PHONY: games

games: $(GAMES_STAMP)

# ---- Rockbox port (sdcard/progs/rockbox.bin) -------------------------------
# `make rockbox` -> builds the Rockbox port via tools/rockbox-port/build.ps1
# (MSYS2 bash wrapper around build.sh: stages the tools/rockbox-port overlay
# into the Rockbox clone, applies the marker-guarded patches, configures +
# builds out-of-tree, copies rockbox.bin to sdcard/progs/). Gated on the
# overlay sources (firmware/ + apps/ + patches/ + the build scripts) being
# newer than the staged binary — build.sh full-cleans its build dir every run,
# so without this gate `make sdcard` would rebuild Rockbox on every invocation.
# Prereqs: MSYS2 bash (C:\msys64), the Rockbox clone at build/rockbox (auto-cloned
# by build.sh), arm toolchain junction C:\arm-gcc (auto-created by build.ps1).
ROCKBOX_OVERLAY := $(wildcard tools/rockbox-port/firmware/target/arm/sc6530c/*) \
                   $(wildcard tools/rockbox-port/firmware/export/*) \
                   $(wildcard tools/rockbox-port/firmware/export/config/*) \
                   $(wildcard tools/rockbox-port/apps/keymaps/*) \
                   tools/rockbox-port/build.sh tools/rockbox-port/build.ps1 \
                   tools/rockbox-port/patches/PATCHES.md

$(SD_CARD_DIR)/progs/rockbox.bin: $(ROCKBOX_OVERLAY)
	$(RUN_ROCKBOX)

# .rockbox/ = Rockbox's runtime data dir on the volume root (codecs/langs/
# fonts/themes/wps/eqs + the .ignore files). build.sh packages it via `make
# zip` (rockbox.zip in the clone's build dir) and extracts it onto the card;
# this rule re-extracts when rockbox.bin is (re)built and makes `sdcard` fail
# if the data dir is missing. The extracted zip entry (rockbox-info.txt) is
# the marker.
$(SD_CARD_DIR)/.rockbox/rockbox-info.txt: $(SD_CARD_DIR)/progs/rockbox.bin
ifeq ($(OS),Windows_NT)
	@powershell -NoProfile -Command "$$z=Join-Path '$(CURDIR)' 'build\rockbox\build-b310e\rockbox.zip'; if (-not (Test-Path $$z)) { Write-Error 'rockbox.zip missing - run make rockbox first'; exit 1 }; New-Item -ItemType Directory -Force -Path '$(CURDIR)/$(SD_CARD_DIR)' | Out-Null; Remove-Item -Recurse -Force '$(CURDIR)/$(SD_CARD_DIR)/.rockbox' -ErrorAction SilentlyContinue; & tar -xf $$z -C '$(CURDIR)/$(SD_CARD_DIR)' '.rockbox'; if ($$LASTEXITCODE -ne 0) { exit 1 }; exit 0"
else
	@z='$(CURDIR)/build/rockbox/build-b310e/rockbox.zip'; if [ ! -f "$$z" ]; then echo 'rockbox.zip missing - run make rockbox first' >&2; exit 1; fi; mkdir -p '$(CURDIR)/$(SD_CARD_DIR)'; rm -rf '$(CURDIR)/$(SD_CARD_DIR)/.rockbox'; unzip -q -o "$$z" '.rockbox/*' -d '$(CURDIR)/$(SD_CARD_DIR)'
endif
	@$(call MSG,sdcard: staged .rockbox/ - Rockbox runtime data (codecs/langs/fonts/themes/wps))

.PHONY: rockbox

rockbox: $(SD_CARD_DIR)/progs/rockbox.bin

.PHONY: sdcard stockram stockram-diag

# The game-data folders (games/<game>/) are created empty - the actual files
# (WADs/ROMs/GRPs) are NOT shipped; the user copies them from F:\games per
# games/README.md. games/README.md is the tracked template (tools/fpmain/).
GAME_DIRS := doom1 doom2 duke3d sw heretic hexen retris snes gameboy nes

sdcard: $(SD_CARD_DIR)/fpbin/fpmain.bin $(SD_CARD_DIR)/fpbin/config.json $(SD_CARD_DIR)/progs/os.bin $(SD_CARD_DIR)/progs/os-dsp-boot.bin $(GAMES_STAMP) $(SD_CARD_DIR)/progs/rockbox.bin $(SD_CARD_DIR)/.rockbox/rockbox-info.txt $(SD_CARD_DIR)/games/README.md

$(SD_CARD_DIR)/games/README.md: tools/fpmain/games-README.md
ifeq ($(OS),Windows_NT)
	@powershell -NoProfile -Command "$$d='$(CURDIR)/$(SD_CARD_DIR)/games'; New-Item -ItemType Directory -Force -Path $$d,$$d/doom1,$$d/doom2,$$d/duke3d,$$d/sw,$$d/heretic,$$d/hexen,$$d/retris,$$d/snes,$$d/gameboy,$$d/nes | Out-Null; Copy-Item -Force '$(CURDIR)/tools/fpmain/games-README.md' "$$d/README.md"; exit 0"
else
	@d='$(CURDIR)/$(SD_CARD_DIR)/games'; mkdir -p "$$d"/doom1 "$$d"/doom2 "$$d"/duke3d "$$d"/sw "$$d"/heretic "$$d"/hexen "$$d"/retris "$$d"/snes "$$d"/gameboy "$$d"/nes; cp -f '$(CURDIR)/tools/fpmain/games-README.md' "$$d/README.md"
endif
	@$(call MSG,sdcard: created games/ folders + README (copy game files from F:\games per the README))

# The PORTED boot menu (tools/fpmain/fpmain.bin) is NOT built by the repo
# toolchain - it is built in the fpdoom clone (tools/fpmain/build-fpmain.ps1
# stages our sources over fpdoom's fpmenu app, builds with the fpdoom
# framework, copies fpmain.bin + configs back here AND onto the card, then
# restores the clone). `make sdcard` runs it when the binary is missing or
# the port sources are newer. Prereq: the fpdoom clone at build/fpdoom
# (auto-cloned by the script; docs/sdboot.md). Rebuilds are timestamp-gated on
# the port sources, not the clone, so a stale clone cannot silently wedge the
# card image.
FPM_SRCS := tools/fpmain/main.c tools/fpmain/menu_stub.s \
            tools/fpmain/font8x16.h tools/fpmain/font5x7.h \
            tools/fpmain/readconf.h tools/fpmain/jsonconf.h \
            tools/fpmain/config.json tools/fpmain/config.txt

tools/fpmain/fpmain.bin: $(FPM_SRCS)
	$(RUN_FPMAIN)

$(SD_CARD_DIR)/fpbin/fpmain.bin: tools/fpmain/fpmain.bin
	@$(call MKDIR_CMD,$(CURDIR)/$(SD_CARD_DIR)/fpbin)
	@$(call COPY_CMD,$(CURDIR)/tools/fpmain/fpmain.bin,$(CURDIR)/$(SD_CARD_DIR)/fpbin/fpmain.bin)
	@echo sdcard: staged fpbin/fpmain.bin - the PORTED boot menu from tools/fpmain/

$(SD_CARD_DIR)/fpbin/config.json: tools/fpmain/config.json
	@$(call MKDIR_CMD,$(CURDIR)/$(SD_CARD_DIR)/fpbin)
	@$(call COPY_CMD,$(CURDIR)/tools/fpmain/config.json,$(CURDIR)/$(SD_CARD_DIR)/fpbin/config.json)
	@$(call MSG,sdcard: staged fpbin/config.json - the PORTS menu (JSON; games need their fpbin/*.bin + games/ dirs))

# config.txt (the legacy line-based PORTS menu) is NOT staged by default -
# it's a fallback the user can find and copy manually if wanted:
#   Copy-Item tools/fpmain/config.txt sdcard/fpbin/config.txt
# $(SD_CARD_DIR)/fpbin/config.txt: tools/fpmain/config.txt
# 	@powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '$(CURDIR)/$(SD_CARD_DIR)/fpbin' | Out-Null; Copy-Item -Force '$(CURDIR)/tools/fpmain/config.txt' '$(CURDIR)/$(SD_CARD_DIR)/fpbin/config.txt'; exit 0"
# 	@echo sdcard: staged fpbin/config.txt - the PORTS menu (games need their fpbin/*.bin + games/ dirs)

$(SD_CARD_DIR)/progs/os.bin: $(OS_SD_TARGET).bin
	@$(call MKDIR_CMD,$(CURDIR)/$(SD_CARD_DIR)/progs)
	@$(call COPY_CMD,$(CURDIR)/$(OS_SD_TARGET).bin,$(CURDIR)/$(SD_CARD_DIR)/progs/os.bin)
	@echo sdcard: staged progs/os.bin - os-sd.bin, the USB-free SD-boot OS - copy the whole sdcard/ folder onto a FAT32 card

$(SD_CARD_DIR)/progs/os-dsp-boot.bin: $(DSPBOOT_TARGET).bin
	@$(call MKDIR_CMD,$(CURDIR)/$(SD_CARD_DIR)/progs)
	@$(call COPY_CMD,$(CURDIR)/$(DSPBOOT_TARGET).bin,$(CURDIR)/$(SD_CARD_DIR)/progs/os-dsp-boot.bin)
	@$(call MSG,sdcard: staged progs/os-dsp-boot.bin - the DSP boot/audio test image (make debug builds it))

# ---- stock firmware RAM-boot (stock-ram.bin) ------------------------------
# `make stockram` -> stock-ram.bin: packages the first 1MB of the ORIGINAL
# firmware (dump_firmware.bin) behind an MMU-alias shim. Loaded with
# `spd_dump fdl nor_fdl1.bin 0x40004000 fdl stock-ram.bin ram`, the shim
# maps VA 0x0-0x100000 (the stock XIP flash window) + VA 0x04000000 (the
# stock OS PSRAM window) onto PSRAM and boots the stock main OS from RAM.
# Requires dump_firmware.bin in the repo root (gitignored). See docs/stockram.md.
.PHONY: stockram

stockram:
	@$(RUN_STOCKRAM)

# `make stockram-diag` -> stock-ram-diag.bin: stock-ram.bin plus a
# diagnostic stub spliced over the stock boot vector (see docs/stockram.md
# marker table) to pinpoint where the stock OS stops after entry.
.PHONY: stockram-diag

stockram-diag:
	@$(RUN_STOCKRAM_DIAG)

# ---- host-side unit tests -------------------------------------------------
HOSTTEST_FLAGS := -std=c11 -Wall -Wextra -DHOST_TEST -Iinclude -Ikernel
HOSTTEST_SRCS  := tests/hosttest.c kernel/ctx_host.S $(wildcard kernel/*.c)

build/$(HOSTTEST_EXE): $(HOSTTEST_SRCS)
	-@$(call MKDIR_CMD,$(CURDIR)/build)
	$(HOSTCC) $(HOSTTEST_FLAGS) -o $@ $(HOSTTEST_SRCS)

hosttest: build/$(HOSTTEST_EXE)
	@$(CURDIR)/build/$(HOSTTEST_EXE)

# ---- acceptance gate: host tests + ARM image assertions --------------------
# (full paths: recipe pipelines run under cmd, which drops the UNC cwd)
check: hosttest $(TARGET).elf
	@$(CHECK_ENTRY)
	@$(CHECK_TEXT)
	@echo check: ALL PASS

clean:
	-@$(CLEAN)
	@$(call RM_RECURSIVE,$(CURDIR)/build)
	@$(RM_SDCARD_GENERATED)
