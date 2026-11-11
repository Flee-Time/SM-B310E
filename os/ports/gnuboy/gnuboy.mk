# gnuboy (Game Boy, Game Boy Color) as a B310E OS app: $(BUILD)/ports/gnuboy.app
# (included by os/Makefile after the kit; `make gnuboy`, and part of `make`
# once the source is there).
#
# The source is upstream gnuboy, pinned and unchanged in build/gnuboy
# (python os/ports/sources.py gnuboy). Ours (os/ports/gnuboy): its system
# interface on the kit (sys_kit.c) and the app's OS side (gnuboy_os.c).
#
# SPDX-License-Identifier: GPL-2.0-or-later

GNUBOY_DIR ?= ../build/gnuboy
GNUBOY_OUT := $(BUILD)/ports/gnuboy
GNUBOY_PORT := ports/gnuboy

# upstream's Rules (less its sys/ back ends)
GNUBOY_SRC := lcd refresh lcdc palette cpu mem rtc hw sound events keytable menu \
	loader save debug emu main rccmds rckeys rcvars rcfile exports split path \
	inflate miniz_tinfl xz/xz_crc32 xz/xz_crc64 xz/xz_dec_lzma2 xz/xz_dec_stream \
	xz/xz_dec_bcj
# the CPU, the LCD and the sound run all the time: ARM at -O2; the rest Thumb -Os
GNUBOY_ARM := cpu lcd refresh lcdc sound mem hw
GNUBOY_OBJS := $(GNUBOY_SRC:%=$(GNUBOY_OUT)/%.o) $(GNUBOY_OUT)/port/sys_kit.o \
	$(GNUBOY_OUT)/port/gnuboy_os.o $(KIT_OBJS)
GNUBOY_CFLAGS := $(KIT_CFLAGS) -std=gnu99 -DIS_LITTLE_ENDIAN -I$(GNUBOY_DIR) \
	-Dmain=gnuboy_main

ifneq ($(wildcard $(GNUBOY_DIR)/cpu.c),)
all: $(BUILD)/ports/gnuboy.app
endif

gnuboy: $(BUILD)/ports/gnuboy.app
.PHONY: gnuboy

$(GNUBOY_DIR)/cpu.c:
	@echo "gnuboy's source is not in $(GNUBOY_DIR):" >&2
	@echo "run python os/ports/sources.py gnuboy" >&2
	@false

$(GNUBOY_OUT)/%.o: $(GNUBOY_DIR)/%.c | $(GNUBOY_DIR)/cpu.c
	@mkdir -p $(dir $@)
	$(CC) $(GNUBOY_CFLAGS) $(if $(filter $(GNUBOY_ARM),$*),-marm -O2,-Os) -w -MMD -MP -c $< -o $@

$(GNUBOY_OUT)/port/sys_kit.o: $(GNUBOY_PORT)/sys_kit.c $(KIT_DIR)/kit.h | $(GNUBOY_DIR)/cpu.c
	@mkdir -p $(dir $@)
	$(CC) $(GNUBOY_CFLAGS) -Wall -Wno-unused-parameter -MMD -MP -c $< -o $@

$(GNUBOY_OUT)/port/gnuboy_os.o: $(GNUBOY_PORT)/gnuboy_os.c $(KIT_DIR)/kit_app.h sdk/app.h \
		include/os/app.h
	@mkdir -p $(dir $@)
	$(CC) $(KIT_OS_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/ports/gnuboy.elf: $(GNUBOY_OBJS) sdk/app.ld
	$(CC) $(KIT_LDFLAGS) -Wl,--wrap=loader_init -Wl,--wrap=rtc_load_internal -Wl,-Map,$(@:.elf=.map) $(GNUBOY_OBJS) $(KIT_LIBS) -o $@

$(BUILD)/ports/gnuboy.app: $(BUILD)/ports/gnuboy.elf sdk/mkapp.py
	$(PYTHON) sdk/mkapp.py $< $@

-include $(GNUBOY_OBJS:.o=.d)
