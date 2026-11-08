# Duke Nukem 3D as a B310E OS app: $(BUILD)/ports/duke3d.app (included by os/Makefile
# after the kit; `make duke3d`, and part of `make` once the source is there).
#
# The sources are upstream's, pinned and unchanged in build/ (python os/ports/sources.py
# jfbuild jfmact jfduke3d jfaudiolib): the Build engine, 3D Realms' MACT library and the
# game, copied to the build with the changes prepare.py lists. Ours (os/ports/duke): the engine's platform layer on the kit, the game's sound on
# the kit's mixer, the app's OS side. README.md here says what licences this makes.
#
# SPDX-License-Identifier: GPL-2.0-or-later

DUKE_BUILD ?= ../build/jfbuild
DUKE_MACT ?= ../build/jfmact
DUKE_GAME ?= ../build/jfduke3d
DUKE_AUDIO ?= ../build/jfaudiolib
DUKE_OUT := $(BUILD)/ports/duke
DUKE_TREE := $(DUKE_OUT)/src
DUKE_PORT := ports/duke

DUKE_ENGINE_SRC := a-c asmprot baselayer cache1d compat crc32 defs engine mmulti_null \
	osd pragmas scriptfile textfont talltextfont smalltextfont version
DUKE_MACT_SRC := util_lib file_lib control keyboard mouse mathutil scriplib animlib
DUKE_GAME_SRC := game actors gamedef global menues player premap sector rts config osdfuncs \
	osdcmds grpscan sounds version
# the drawing: ARM at -O2; the rest Thumb -Os
DUKE_ARM := a-c engine pragmas cache1d
DUKE_OBJS := $(DUKE_ENGINE_SRC:%=$(DUKE_OUT)/engine/%.o) $(DUKE_MACT_SRC:%=$(DUKE_OUT)/mact/%.o) \
	$(DUKE_GAME_SRC:%=$(DUKE_OUT)/game/%.o) $(DUKE_OUT)/port/layer.o $(DUKE_OUT)/port/duke_sound.o \
	$(DUKE_OUT)/port/kplib_none.o $(DUKE_OUT)/port/ldiv.o $(DUKE_OUT)/port/draw4.o \
	$(DUKE_OUT)/port/duke_os.o $(KIT_OBJS)
DUKE_CFLAGS := $(KIT_CFLAGS) -std=gnu99 -DUSE_POLYMOST=0 -DUSE_OPENGL=0 -DUSE_ASM=0 \
	-DB_LITTLE_ENDIAN=1 -DB_BIG_ENDIAN=0 -DB_ENDIAN_C_INLINE=1 -include $(DUKE_PORT)/duke_compat.h \
	-DRENDERTYPESDL=0 -I$(DUKE_TREE)/game -I$(DUKE_TREE)/build/include -I$(DUKE_TREE)/build/src \
	-I$(DUKE_MACT) -I$(DUKE_AUDIO)/include -I$(KIT_DIR) -I$(KIT_DIR)/sound

ifneq ($(wildcard $(DUKE_GAME)/src/game.c),)
ifneq ($(wildcard $(DUKE_BUILD)/src/engine.c),)
all: $(BUILD)/ports/duke3d.app
endif
endif

duke3d: $(BUILD)/ports/duke3d.app
.PHONY: duke3d

$(DUKE_TREE)/.prepared: $(DUKE_PORT)/prepare.py $(DUKE_BUILD)/src/engine.c $(DUKE_GAME)/src/game.c
	$(PYTHON) $(DUKE_PORT)/prepare.py $(DUKE_BUILD) $(DUKE_GAME) $(DUKE_TREE)

$(DUKE_ENGINE_SRC:%=$(DUKE_TREE)/build/src/%.c) $(DUKE_GAME_SRC:%=$(DUKE_TREE)/game/%.c): $(DUKE_TREE)/.prepared ;

$(DUKE_ENGINE_SRC:%=$(DUKE_OUT)/engine/%.o): $(DUKE_OUT)/engine/%.o: $(DUKE_TREE)/build/src/%.c $(DUKE_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(CC) $(DUKE_CFLAGS) $(if $(filter $(DUKE_ARM),$*),-marm -O2,-Os) -w -MMD -MP -c $< -o $@

$(DUKE_MACT_SRC:%=$(DUKE_OUT)/mact/%.o): $(DUKE_OUT)/mact/%.o: $(DUKE_MACT)/%.c $(DUKE_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(CC) $(DUKE_CFLAGS) -Os -w -MMD -MP -c $< -o $@

$(DUKE_GAME_SRC:%=$(DUKE_OUT)/game/%.o): $(DUKE_OUT)/game/%.o: $(DUKE_TREE)/game/%.c $(DUKE_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(CC) $(DUKE_CFLAGS) -Os -w -MMD -MP -c $< -o $@

$(DUKE_OUT)/port/layer.o $(DUKE_OUT)/port/duke_sound.o $(DUKE_OUT)/port/kplib_none.o: $(DUKE_OUT)/port/%.o: $(DUKE_PORT)/%.c $(KIT_DIR)/kit.h \
		$(DUKE_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(CC) $(DUKE_CFLAGS) -Wall -Wno-unused-parameter -Wno-sign-compare -MMD -MP -c $< -o $@

$(DUKE_OUT)/port/ldiv.o $(DUKE_OUT)/port/draw4.o: $(DUKE_OUT)/port/%.o: $(DUKE_PORT)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ARCH_FLAGS) -marm -c $< -o $@

$(DUKE_OUT)/port/duke_os.o: $(DUKE_PORT)/duke_os.c $(KIT_DIR)/kit_app.h sdk/app.h include/os/app.h
	@mkdir -p $(dir $@)
	$(CC) $(KIT_OS_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/ports/duke3d.elf: $(DUKE_OBJS) sdk/app.ld
	$(CC) $(KIT_LDFLAGS) -Wl,--wrap=Bgetsysmemsize -Wl,--wrap=__aeabi_ldivmod \
		-Wl,--wrap=drawrooms -Wl,--wrap=drawmasks -Wl,-Map,$(@:.elf=.map) $(DUKE_OBJS) $(KIT_LIBS) -o $@

$(BUILD)/ports/duke3d.app: $(BUILD)/ports/duke3d.elf sdk/mkapp.py
	$(PYTHON) sdk/mkapp.py $< $@

-include $(DUKE_OBJS:.o=.d)
