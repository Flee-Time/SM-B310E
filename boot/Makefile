# B310E boot loader (NOR) — build with arm-none-eabi-gcc (GNU make).
#
#   make                 $(BUILD)/b310e-boot.bin (no key: the menu)
#   make DEFAULT=stock   no key boots stock (stock|menu|program[:path])
#
# The menu draws with B310E OS's own ui/gfx.c, Montserrat font and icons.
#
# SPDX-License-Identifier: GPL-2.0-or-later

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size
PYTHON  ?= python3
BUILD   ?= ../build/boot
FATFS   ?= ../build/fatfs/source
DEFAULT ?= menu

ARCH_FLAGS := -march=armv5te -mtune=arm926ej-s -mthumb-interwork
CFLAGS  := $(ARCH_FLAGS) -mthumb -Os -g -std=gnu11 -ffreestanding -fno-builtin \
           -Wall -Wextra -Wno-unused-parameter -fno-common \
           -ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns \
           -I. -I$(FATFS) -I../os -I../os/include
ASFLAGS := $(ARCH_FLAGS) -marm -g -I.
LDFLAGS := $(ARCH_FLAGS) -nostdlib -T boot.ld -Wl,--gc-sections \
           -Wl,-Map,$(BUILD)/b310e-boot.map -Wl,--no-warn-rwx-segments

OBJS := $(BUILD)/start.o $(BUILD)/hw.o $(BUILD)/lcd.o $(BUILD)/sd.o \
        $(BUILD)/diskio.o $(BUILD)/lib.o $(BUILD)/main.o $(BUILD)/battery.o \
        $(BUILD)/ui/gfx.o $(BUILD)/ui/font_montserrat.o $(BUILD)/ui/icons.o \
        $(BUILD)/fatfs/ff.o $(BUILD)/fatfs/ffunicode.o

all: $(BUILD)/b310e-boot.bin

$(BUILD)/%.o: %.c boot.h ffconf.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S boot.h
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/ui/%.o: ../os/ui/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/fatfs/%.o: $(FATFS)/%.c ffconf.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

$(BUILD)/b310e-boot.elf: $(OBJS) boot.ld
	$(CC) $(LDFLAGS) $(OBJS) -lgcc -o $@
	$(SIZE) $@

$(BUILD)/b310e-boot-raw.bin: $(BUILD)/b310e-boot.elf
	$(OBJCOPY) -O binary $< $@

# The header's default and CRC-32 (mkboot.py also checks the layout).
$(BUILD)/b310e-boot.bin: $(BUILD)/b310e-boot-raw.bin mkboot.py FORCE
	$(PYTHON) mkboot.py image $< --default $(DEFAULT) --output $@

clean:
	rm -rf $(BUILD)

FORCE:
.PHONY: all clean FORCE

-include $(OBJS:.o=.d)
