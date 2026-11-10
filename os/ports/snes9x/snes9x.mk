# The SNES as a B310E OS app: $(BUILD)/ports/snes9x.app (included by os/Makefile after the
# kit; `make snes9x`, and part of `make` once the source is there).
#
# The source is libretro's snes9x2002 (PocketSNES's Snes9x, with its ARM assembly),
# pinned and unchanged in build/snes9x2002 (python os/ports/sources.py snes9x2002),
# copied to the build with the changes prepare.py lists. Its licence is Snes9x's (free,
# not for commercial use, not GPL): nothing of it is in the repository, and the built
# app is for the user's own phone. Ours (os/ports/snes9x): a libretro frontend on the
# kit (snes_kit.c) and the app's OS side (snes9x_os.c).
#
# SPDX-License-Identifier: GPL-2.0-or-later

SNES_DIR ?= ../build/snes9x2002
# the CPU and SPC700 cores, each kind's objects apart: spc, the C CPU and the ARM
# assembly SPC700 (prepare.py 6); c, both C (upstream's miyoo build); asm, both
# assembly (not right yet: prepare.py 6); cr, spc with upstream's C renderer
# (a reference for the ARM one)
SNES_CORES ?= spc
SNES_OUT := $(BUILD)/ports/snes9x$(if $(filter-out asm,$(SNES_CORES)),-$(SNES_CORES))
SNES_TREE := $(SNES_OUT)/src
SNES_PORT := ports/snes9x

# upstream's Makefile.common, as its ARM926 build (miyoo) takes it: the ARM renderer,
# its C CPU and SPC700 (which SNES_CORES may replace)
SNES_CORE := apu apuaux c4 c4emu cheats cheats2 clip data dsp1 fxemu fxinst globals dma \
	memmap cpu cpuexec cpuops sa1 sa1cpu sdd1 sdd1emu snapshot soundux spc700 srtc \
	ppu rops mode7new mode7 mode7add mode7add1_2 mode7sub mode7sub1_2 mode7prio \
	mode7addprio mode7add1_2prio mode7subprio mode7sub1_2prio gfx16 tile16 tile16add \
	tile16add1_2 tile16fadd1_2 tile16sub tile16sub1_2 tile16fsub1_2
ifeq ($(SNES_CORES),cr)                 # (a reference: upstream's C renderer too)
SNES_CORE := $(filter-out ppu rops mode7% gfx16 tile16%,$(SNES_CORE)) ppu_ gfx tile
endif
SNES_SRCS := $(SNES_CORE:%=src/%) libretro/libretro libretro/libretro-common/streams/memory_stream \
	$(if $(filter asm,$(SNES_CORES)),src/os9x_asm_cpu)
# its ARM assembly 65816 and SPC700 (upstream's ASM_CPU, ASM_SPC700; prepare.py 6)
SNES_ASM := $(if $(filter asm,$(SNES_CORES)),src/os9x_65c816_global src/os9x_65c816_spcasm) \
	$(if $(filter asm spc cr,$(SNES_CORES)),src/spc700a src/spc_decode)
SNES_DEFS := $(if $(filter asm,$(SNES_CORES)),-DASMCPU) $(if $(filter asm spc cr,$(SNES_CORES)),-DASM_SPC700)
SNES_OBJS := $(SNES_SRCS:%=$(SNES_OUT)/%.o) $(SNES_ASM:%=$(SNES_OUT)/%.o) $(SNES_OUT)/port/snes_kit.o \
	$(SNES_OUT)/port/snes9x_os.o $(KIT_OBJS)
SNES_CFLAGS := $(filter-out -std=gnu11,$(KIT_CFLAGS)) -std=gnu99 -I$(SNES_TREE) -I$(SNES_TREE)/src \
	-I$(SNES_TREE)/libretro -I$(SNES_TREE)/libretro/libretro-common/include \
	$(if $(filter cr,$(SNES_CORES)),-D__OLD_RASTER_FX__,-DARM_ASM) $(SNES_DEFS) -DRIGHTSHIFT_IS_SAR -DFAST_ALIGNED_LSB_WORD_ACCESS -DHAVE_STRINGS_H \
	-DHAVE_STDINT_H -DHAVE_INTTYPES_H -D__LIBRETRO__ -DUSE_SA1 -fsigned-char -ffast-math \
	-Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types \
	-Wno-error=int-conversion -fcommon -g

ifneq ($(wildcard $(SNES_DIR)/src/cpuexec.c),)
all: $(BUILD)/ports/snes9x.app
endif

snes9x: $(BUILD)/ports/snes9x.app
.PHONY: snes9x

$(SNES_DIR)/src/cpuexec.c:
	@echo "snes9x2002's source is not in $(SNES_DIR):" >&2
	@echo "run python os/ports/sources.py snes9x2002" >&2
	@false

$(SNES_TREE)/.prepared: $(SNES_PORT)/prepare.py | $(SNES_DIR)/src/cpuexec.c
	$(PYTHON) $(SNES_PORT)/prepare.py $(SNES_DIR) $(SNES_TREE)

$(SNES_SRCS:%=$(SNES_TREE)/%.c): $(SNES_TREE)/.prepared ;

# the emulation runs all the time: ARM, -O2
$(SNES_SRCS:%=$(SNES_OUT)/%.o): $(SNES_OUT)/%.o: $(SNES_TREE)/%.c $(SNES_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(CC) $(SNES_CFLAGS) -marm -O2 -w -MMD -MP -c $< -o $@

$(SNES_ASM:%=$(SNES_TREE)/%.S): $(SNES_TREE)/.prepared ;

$(SNES_ASM:%=$(SNES_OUT)/%.o): $(SNES_OUT)/%.o: $(SNES_TREE)/%.S $(SNES_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(CC) $(SNES_CFLAGS) -marm -I$(SNES_TREE)/src -c $< -o $@

$(SNES_OUT)/port/snes_kit.o:$(SNES_PORT)/snes_kit.c $(KIT_DIR)/kit.h $(SNES_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(CC) $(SNES_CFLAGS) -O2 -Wall -Wno-unused-parameter -MMD -MP -c $< -o $@

$(SNES_OUT)/port/snes9x_os.o: $(SNES_PORT)/snes9x_os.c $(KIT_DIR)/kit_app.h sdk/app.h include/os/app.h
	@mkdir -p $(dir $@)
	$(CC) $(KIT_OS_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/ports/snes9x.elf: $(SNES_OBJS) sdk/app.ld
	$(CC) $(KIT_LDFLAGS) -Wl,-Map,$(@:.elf=.map) $(SNES_OBJS) $(KIT_LIBS) -o $@

$(BUILD)/ports/snes9x.app: $(BUILD)/ports/snes9x.elf sdk/mkapp.py
	$(PYTHON) sdk/mkapp.py $< $@

-include $(SNES_OBJS:.o=.d)
