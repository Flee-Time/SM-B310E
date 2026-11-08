# Wolf4SDL (Wolfenstein 3D) as a B310E OS app: $(BUILD)/ports/wolf3d.app
# (included by os/Makefile after the kit; `make wolf3d`, and part of `make`
# once the source is there).
#
# The source is upstream Wolf4SDL, pinned and unchanged in build/wolf4sdl
# (python os/ports/sources.py wolf4sdl), copied to the build with one
# change (prepare.py: no global #pragma pack(1), which this ARMv5 cannot
# take; the two map-file structs packed one by one), under the GPL (its other licence,
# id's, is not used): the music and AdLib effects by DOSBox's GPL OPL
# emulator (USE_GPL), not MAME's. With it id_sd.c is C++, so the game is
# built as C++ throughout, as Wolf4SDL's Visual C++ projects do. Ours
# (os/ports/wolf4sdl): an SDL and SDL_mixer of our own on the kit (sdl/,
# sdl_kit.c: only what Wolf4SDL uses), main (wolf_glue.cpp), the keys (wolf_keys.c),
# the app's OS side (wolf_os.c). The edition is Wolf4SDL's default: the
# registered game, GoodTimes/Activision 1.4 (*.WL6).
#
# SPDX-License-Identifier: GPL-2.0-or-later

WOLF_DIR ?= ../build/wolf4sdl
WOLF_OUT := $(BUILD)/ports/wolf4sdl
WOLF_TREE := $(WOLF_OUT)/src
WOLF_PORT := ports/wolf4sdl
WOLF_CXX := $(CROSS)g++

WOLF_SRC := id_ca id_in id_pm id_sd id_us id_vh id_vl signon wl_act1 wl_act2 wl_agent \
	wl_atmos wl_cloudsky wl_debug wl_draw wl_game wl_inter wl_main wl_menu wl_parallax \
	wl_plane wl_play wl_scale wl_shade wl_state wl_text wl_utils
# the walls, the sprites, the actors and the OPL: ARM at -O2; the rest Thumb -Os
WOLF_ARM := wl_draw wl_scale wl_act1 wl_act2 wl_agent wl_state wl_play id_vl id_vh id_sd
WOLF_OBJS := $(WOLF_SRC:%=$(WOLF_OUT)/%.o) $(WOLF_OUT)/dosbox/dbopl.o \
	$(WOLF_OUT)/port/sdl_kit.o $(WOLF_OUT)/port/wolf_keys.o $(WOLF_OUT)/port/wolf_glue.o \
	$(WOLF_OUT)/port/wolf_os.o \
	$(KIT_OBJS)
WOLF_CXXFLAGS := $(filter-out -std=gnu11,$(KIT_CFLAGS)) -std=c++11 -D_POSIX_C_SOURCE=200809L -fno-exceptions -fno-rtti \
	-fno-threadsafe-statics -fno-use-cxa-atexit -fpermissive -DUSE_GPL -Dmain=wolf_main \
	-I$(WOLF_PORT)/sdl -I$(WOLF_TREE)

ifneq ($(wildcard $(WOLF_DIR)/wl_main.c),)
all: $(BUILD)/ports/wolf3d.app
endif

wolf3d: $(BUILD)/ports/wolf3d.app
.PHONY: wolf3d

$(WOLF_DIR)/wl_main.c:
	@echo "Wolf4SDL's source is not in $(WOLF_DIR):" >&2
	@echo "run python os/ports/sources.py wolf4sdl" >&2
	@false

# upstream's tree, copied with the pack(1) change (prepare.py says why)
$(WOLF_TREE)/.prepared: $(WOLF_PORT)/prepare.py | $(WOLF_DIR)/wl_main.c
	$(PYTHON) $(WOLF_PORT)/prepare.py $(WOLF_DIR) $(WOLF_TREE)

$(WOLF_SRC:%=$(WOLF_TREE)/%.c) $(WOLF_TREE)/dosbox/dbopl.cpp: $(WOLF_TREE)/.prepared ;

$(WOLF_SRC:%=$(WOLF_OUT)/%.o): $(WOLF_OUT)/%.o: $(WOLF_TREE)/%.c $(WOLF_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(WOLF_CXX) -x c++ $(WOLF_CXXFLAGS) $(if $(filter $(WOLF_ARM),$*),-marm -O2,-Os) -w \
		-MMD -MP -c $< -o $@

$(WOLF_OUT)/dosbox/dbopl.o: $(WOLF_TREE)/dosbox/dbopl.cpp $(WOLF_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(WOLF_CXX) $(WOLF_CXXFLAGS) -marm -O2 -w -MMD -MP -c $< -o $@

$(WOLF_OUT)/port/sdl_kit.o $(WOLF_OUT)/port/wolf_keys.o: $(WOLF_OUT)/port/%.o: $(WOLF_PORT)/%.c $(WOLF_PORT)/sdl/SDL.h \
		$(WOLF_PORT)/sdl/SDL_mixer.h $(KIT_DIR)/kit.h
	@mkdir -p $(dir $@)
	$(CC) $(KIT_CFLAGS) -marm -O2 -I$(WOLF_PORT)/sdl -Wall -Wextra -Wno-unused-parameter \
		-MMD -MP -c $< -o $@

$(WOLF_OUT)/port/wolf_glue.o: $(WOLF_PORT)/wolf_glue.cpp $(WOLF_PORT)/sdl/SDL.h $(KIT_DIR)/kit.h 		$(WOLF_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(WOLF_CXX) $(WOLF_CXXFLAGS) -Wall -MMD -MP -c $< -o $@

$(WOLF_OUT)/port/wolf_os.o: $(WOLF_PORT)/wolf_os.c $(KIT_DIR)/kit_app.h sdk/app.h include/os/app.h
	@mkdir -p $(dir $@)
	$(CC) $(KIT_OS_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/ports/wolf3d.elf: $(WOLF_OBJS) sdk/app.ld
	$(CC) $(KIT_LDFLAGS) -Wl,-Map,$(@:.elf=.map) $(WOLF_OBJS) $(KIT_LIBS) -o $@

$(BUILD)/ports/wolf3d.app: $(BUILD)/ports/wolf3d.elf sdk/mkapp.py
	$(PYTHON) sdk/mkapp.py $< $@

-include $(WOLF_OBJS:.o=.d)
