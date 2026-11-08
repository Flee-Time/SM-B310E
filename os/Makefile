# B310E OS — build with arm-none-eabi-gcc (GNU make).
#
#   make                 build $(BUILD)/b310e-os.bin
#   make BUILD=dir       put objects and the image elsewhere
#
# SPDX-License-Identifier: GPL-2.0-or-later

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size
BUILD   ?= ../build/os
# FatFs R0.15a sources (fetched by scripts/targets/os.sh, never ffconf.h:
# ours is fs/ffconf.h)
FATFS   ?= ../build/fatfs/source

ARCH_FLAGS := -march=armv5te -mtune=arm926ej-s -mthumb-interwork
CFLAGS  := $(ARCH_FLAGS) -mthumb -Os -g -std=gnu11 -ffreestanding \
           -Wall -Wextra -Wno-unused-parameter -fno-common \
           -ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns \
           -I. -Iinclude -Ifs -I$(FATFS) -I$(BUILD)
ASFLAGS := $(ARCH_FLAGS) -marm -g -Iinclude
LDFLAGS := $(ARCH_FLAGS) -nostdlib -T link/os.ld -Wl,--gc-sections \
           -Wl,-Map,$(BUILD)/b310e-os.map -Wl,--no-warn-rwx-segments
CFLAGS  += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)

SRCS_S := $(wildcard arch/sc6530c/*.S)
SRCS_C := $(wildcard arch/sc6530c/*.c kernel/*.c drivers/*.c fs/*.c lib/*.c \
          loader/*.c ui/*.c apps/*.c net/*.c media/*.c) main.c selftest.c
OBJS   := $(addprefix $(BUILD)/,$(SRCS_S:.S=.o) $(SRCS_C:.c=.o)) \
          $(BUILD)/fatfs/ff.o $(BUILD)/fatfs/ffunicode.o

# Loadable apps: every folder in modules/ becomes $(BUILD)/modules/<folder>.app
# (os/sdk: app.h, app.ld, start.c, mkapp.py).
PYTHON      ?= python3
APP_NAMES   := $(notdir $(patsubst %/,%,$(wildcard modules/*/)))
APPS        := $(APP_NAMES:%=$(BUILD)/modules/%.app)
APP_CFLAGS  := $(CFLAGS) -Isdk -fno-unwind-tables -fno-asynchronous-unwind-tables
# --pic-veneer: the linker's ARM/Thumb stubs (libgcc's soft-float and
# division are ARM code) must be PC-relative, as an app runs wherever the
# loader puts it; the default stubs load an absolute address mkapp.py does
# not see (the first calculator app jumped to 0x125d, 2026-11-07).
APP_LDFLAGS := $(ARCH_FLAGS) -nostdlib -T sdk/app.ld -Wl,-q -Wl,--gc-sections \
               -Wl,--pic-veneer -Wl,--no-warn-rwx-segments

all: $(BUILD)/b310e-os.bin $(APPS)

# Codec modules (media/codecs.mk): $(BUILD)/codecs/<name>.cdc
include media/codecs.mk

# Ports of games as apps (ports/doom/doom.mk): $(BUILD)/ports/doom.app
include ports/kit/kit.mk
include ports/chocolate/chocolate.mk
include ports/gnuboy/gnuboy.mk
include ports/infones/infones.mk
include ports/wolf4sdl/wolf4sdl.mk
include ports/duke/duke.mk

.SECONDEXPANSION:
.PRECIOUS: $(BUILD)/modules/%.elf
$(BUILD)/modules/%.elf: $$(wildcard modules/$$*/*.c) sdk/start.c sdk/app.ld sdk/app.h \
                     include/os/app.h include/os/ui.h
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) $(APP_LDFLAGS) -Wl,-Map,$(@:.elf=.map) \
	    $(filter %.c,$^) -lgcc -o $@

$(BUILD)/modules/%.app: $(BUILD)/modules/%.elf sdk/mkapp.py
	$(PYTHON) sdk/mkapp.py $< $@

$(BUILD)/build_info.h: FORCE
	@mkdir -p $(dir $@)
	@echo '#define BUILD_DATE "'"$$(date '+%Y-%m-%d %H:%M')"'"' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@; rm -f $@.tmp

$(BUILD)/%.o: %.c $(BUILD)/build_info.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/fatfs/%.o: $(FATFS)/%.c fs/ffconf.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/b310e-os.elf: $(OBJS) link/os.ld
	$(CC) $(LDFLAGS) $(OBJS) -lgcc -o $@
	$(SIZE) $@

$(BUILD)/b310e-os.bin: $(BUILD)/b310e-os.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILD)

FORCE:
.PHONY: all clean FORCE

-include $(OBJS:.o=.d)
