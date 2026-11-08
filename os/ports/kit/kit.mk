# The game kit (os/ports/kit), included by os/Makefile before the ports
# that use it. A port links $(KIT_OBJS) and its own objects with
# $(KIT_LDFLAGS) ... $(KIT_LIBS):
#   - game side: KIT_CFLAGS (newlib's headers and C library, Thumb)
#   - OS side:   KIT_OS_CFLAGS (the SDK's app.h, the API's names)
#
# SPDX-License-Identifier: GPL-2.0-or-later

KIT_DIR := ports/kit
KIT_OUT := $(BUILD)/ports/kit

KIT_CFLAGS := $(ARCH_FLAGS) --specs=nano.specs -mthumb -O2 -std=gnu11 -fno-PIE -fsigned-char \
	-fno-strict-aliasing -fomit-frame-pointer -ffunction-sections -fdata-sections \
	-fno-unwind-tables -fno-asynchronous-unwind-tables \
	-fno-tree-loop-distribute-patterns -I$(KIT_DIR)/include -I$(KIT_DIR) -I$(KIT_DIR)/sound -I$(KIT_OUT)
KIT_OS_CFLAGS := $(APP_CFLAGS) -I$(KIT_DIR)
# no unwind tables in an app (sdk/app.ld drops them): libgcc's unwinder,
# which newlib's objects name, finds none
KIT_LDFLAGS := $(ARCH_FLAGS) --specs=nano.specs -mthumb -nostartfiles -T sdk/app.ld -Wl,-q -Wl,--gc-sections \
	-Wl,--pic-veneer -Wl,--no-warn-rwx-segments \
	-Wl,--defsym=__exidx_start=0 -Wl,--defsym=__exidx_end=0 -Wl,--wrap=fopen -Wl,--wrap=fread
KIT_LIBS := -Wl,--start-group -lc_nano -lm -lgcc -Wl,--end-group

KIT_OBJS := $(KIT_OUT)/kit_os.o $(KIT_OUT)/kit_menu.o $(KIT_OUT)/kit_sys.o $(KIT_OUT)/kit_malloc.o $(KIT_OUT)/kit_asm.o \
	$(KIT_OUT)/b310e-mixer.o $(KIT_OUT)/b310e-music.o

$(KIT_OUT)/kit_os.o: $(KIT_DIR)/kit_os.c $(KIT_DIR)/kit.h $(KIT_DIR)/kit_app.h \
		$(KIT_DIR)/kit_os.h sdk/app.h include/os/app.h include/os/ui.h
	@mkdir -p $(dir $@)
	$(CC) $(KIT_OS_CFLAGS) -marm -O2 -MMD -MP -c $< -o $@   # ARM, -O2: every frame goes through it

$(KIT_OUT)/kit_menu.o: $(KIT_DIR)/kit_menu.c $(KIT_DIR)/kit.h $(KIT_DIR)/kit_app.h \
		sdk/app.h include/os/app.h include/os/ui.h
	@mkdir -p $(dir $@)
	$(CC) $(KIT_OS_CFLAGS) -MMD -MP -c $< -o $@

$(KIT_OUT)/kit_malloc.o: $(KIT_DIR)/kit_malloc.c $(KIT_DIR)/kit.h $(KIT_DIR)/kit_os.h
	@mkdir -p $(dir $@)
	$(CC) $(KIT_CFLAGS) -marm -fno-builtin -Wall -Wextra -Wno-array-bounds -MMD -MP -c $< -o $@  # not calloc for malloc+memset: it is calloc

$(KIT_OUT)/kit_sys.o: $(KIT_DIR)/kit_sys.c $(KIT_DIR)/kit.h $(KIT_DIR)/kit_os.h
	@mkdir -p $(dir $@)
	$(CC) $(KIT_CFLAGS) -Wall -Wextra -Wno-unused-parameter -MMD -MP -c $< -o $@

$(KIT_OUT)/kit_asm.o: $(KIT_DIR)/kit_asm.S
	@mkdir -p $(dir $@)
	$(CC) $(ARCH_FLAGS) -marm -c $< -o $@

# the mixer and the synth: ARM code, they run for every sample
$(KIT_OUT)/b310e-notes.h: $(KIT_DIR)/sound/mknotes.py
	@mkdir -p $(dir $@)
	$(PYTHON) $< $@

$(KIT_OUT)/b310e-mixer.o $(KIT_OUT)/b310e-music.o: $(KIT_OUT)/%.o: $(KIT_DIR)/sound/%.c \
		$(KIT_OUT)/b310e-notes.h $(KIT_DIR)/sound/b310e-audio.h
	$(CC) $(KIT_CFLAGS) -marm -w -MMD -MP -c $< -o $@

-include $(KIT_OBJS:.o=.d)
