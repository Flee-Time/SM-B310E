# A Build engine game as a B310E OS app (included by its own .mk: ports/duke/duke.mk,
# ports/sw/sw.mk). The sources are upstream's, pinned and unchanged in build/ (python
# os/ports/sources.py jfbuild jfmact jfaudiolib and the game's): the Build engine, 3D Realms'
# MACT library and the game, copied to the build with the changes the game's prepare.py
# and ports/build/engine.py list. Ours here: the engine's platform layer on the kit, the
# audiolib on the kit's mixer, the drawing loops, a divide. Each game gives build_game.h's
# hooks and its OS side.
#
# A game sets <id>_GAME (its source), <id>_GAME_SRC (its files), <id>_PORT (its folder),
# <id>_C (its own files), <id>_APP (the .app's name) and then $(eval $(call build_game,<id>)).
# A game in C++ also sets <id>_GAME_CXX (its .cpp files) and <id>_CPP (its own); <id>_MARK
# is a file of its source (src/game.c if not set).
#
# SPDX-License-Identifier: GPL-2.0-or-later

ifndef BUILD_GAME_MK
BUILD_GAME_MK := 1

JFBUILD ?= ../build/jfbuild
JFMACT ?= ../build/jfmact
JFAUDIO ?= ../build/jfaudiolib
BUILD_PORT := ports/build
BUILD_CXX := $(CROSS)g++

BUILD_ENGINE_SRC := a-c asmprot baselayer cache1d compat crc32 defs engine mmulti_null \
	osd pragmas scriptfile textfont talltextfont smalltextfont version
BUILD_MACT_SRC := util_lib file_lib control keyboard mouse mathutil scriplib animlib
BUILD_PORT_C := layer sound kplib_none
BUILD_PORT_S := ldiv draw4
# the drawing: ARM at -O2; the rest Thumb -Os
BUILD_ARM := a-c engine pragmas cache1d

define build_game
$(1)_OUT := $$(BUILD)/ports/$(1)
$(1)_TREE := $$($(1)_OUT)/src
$(1)_MARK_FILE := $$($(1)_GAME)/$$(or $$($(1)_MARK),src/game.c)
$(1)_OBJS := $$(BUILD_ENGINE_SRC:%=$$($(1)_OUT)/engine/%.o) $$(BUILD_MACT_SRC:%=$$($(1)_OUT)/mact/%.o) \
	$$($(1)_GAME_SRC:%=$$($(1)_OUT)/game/%.o) $$(BUILD_PORT_C:%=$$($(1)_OUT)/port/%.o) \
	$$($(1)_GAME_CXX:%=$$($(1)_OUT)/game/%.o) $$($(1)_CPP:%=$$($(1)_OUT)/port/%.o) \
	$$(BUILD_PORT_S:%=$$($(1)_OUT)/port/%.o) $$($(1)_C:%=$$($(1)_OUT)/port/%.o) \
	$$($(1)_OUT)/port/$(1)_os.o $$(KIT_OBJS)
$(1)_CFLAGS := $$(KIT_CFLAGS) -std=gnu99 -DUSE_POLYMOST=0 -DUSE_OPENGL=0 -DUSE_ASM=0 \
	-DB_LITTLE_ENDIAN=1 -DB_BIG_ENDIAN=0 -DB_ENDIAN_C_INLINE=1 -include $$(BUILD_PORT)/compat.h \
	-DRENDERTYPESDL=0 -I$$($(1)_TREE)/game -I$$($(1)_TREE)/build/include -I$$($(1)_TREE)/build/src \
	-I$$(JFMACT) -I$$(JFAUDIO)/include -I$$(KIT_DIR) -I$$(KIT_DIR)/sound -I$$(BUILD_PORT) $$($(1)_DEFS)
$(1)_CXXFLAGS := $$(filter-out -std=gnu99,$$($(1)_CFLAGS)) -std=gnu++14 -fno-exceptions -fno-rtti \
	-fno-threadsafe-statics -fno-use-cxa-atexit -Wno-narrowing $$($(1)_CXXDEFS)

ifneq ($$(wildcard $$($(1)_MARK_FILE)),)
ifneq ($$(wildcard $$(JFBUILD)/src/engine.c),)
all: $$(BUILD)/ports/$$($(1)_APP).app
endif
endif

$$($(1)_APP): $$(BUILD)/ports/$$($(1)_APP).app
.PHONY: $$($(1)_APP)

$$($(1)_TREE)/.prepared: $$($(1)_PORT)/prepare.py $$(BUILD_PORT)/engine.py $$(BUILD_PORT)/unidiff.py \
		$$(JFBUILD)/src/engine.c $$($(1)_MARK_FILE)
	$$(PYTHON) $$($(1)_PORT)/prepare.py $$(JFBUILD) $$($(1)_GAME) $$($(1)_TREE)

$$(BUILD_ENGINE_SRC:%=$$($(1)_TREE)/build/src/%.c) $$($(1)_GAME_SRC:%=$$($(1)_TREE)/game/%.c) \
		$$($(1)_GAME_CXX:%=$$($(1)_TREE)/game/%.cpp): $$($(1)_TREE)/.prepared ;

$$(BUILD_ENGINE_SRC:%=$$($(1)_OUT)/engine/%.o): $$($(1)_OUT)/engine/%.o: $$($(1)_TREE)/build/src/%.c $$($(1)_TREE)/.prepared
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(1)_CFLAGS) $$(if $$(filter $$(BUILD_ARM),$$*),-marm -O2,-Os) -w -MMD -MP -c $$< -o $$@

$$(BUILD_MACT_SRC:%=$$($(1)_OUT)/mact/%.o): $$($(1)_OUT)/mact/%.o: $$(JFMACT)/%.c $$($(1)_TREE)/.prepared
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(1)_CFLAGS) -Os -w -MMD -MP -c $$< -o $$@

$$($(1)_GAME_SRC:%=$$($(1)_OUT)/game/%.o): $$($(1)_OUT)/game/%.o: $$($(1)_TREE)/game/%.c $$($(1)_TREE)/.prepared
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(1)_CFLAGS) $$(if $$(filter $$($(1)_ARM),$$*),-marm -O2,-Os) -w -MMD -MP -c $$< -o $$@

$$($(1)_GAME_CXX:%=$$($(1)_OUT)/game/%.o): $$($(1)_OUT)/game/%.o: $$($(1)_TREE)/game/%.cpp $$($(1)_TREE)/.prepared
	@mkdir -p $$(dir $$@)
	$$(BUILD_CXX) $$($(1)_CXXFLAGS) $$(if $$(filter $$($(1)_ARM),$$*),-marm -O2,-Os) -w -MMD -MP -c $$< -o $$@

$$($(1)_CPP:%=$$($(1)_OUT)/port/%.o): $$($(1)_OUT)/port/%.o: $$($(1)_PORT)/%.cpp $$(KIT_DIR)/kit.h \
		$$(BUILD_PORT)/build_game.h $$($(1)_TREE)/.prepared
	@mkdir -p $$(dir $$@)
	$$(BUILD_CXX) $$($(1)_CXXFLAGS) -Os -Wall -Wno-unused-parameter -Wno-sign-compare -MMD -MP -c $$< -o $$@

$$(BUILD_PORT_C:%=$$($(1)_OUT)/port/%.o): $$($(1)_OUT)/port/%.o: $$(BUILD_PORT)/%.c $$(KIT_DIR)/kit.h \
		$$(BUILD_PORT)/build_game.h $$($(1)_TREE)/.prepared
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(1)_CFLAGS) -Wall -Wno-unused-parameter -Wno-sign-compare -MMD -MP -c $$< -o $$@

$$($(1)_C:%=$$($(1)_OUT)/port/%.o): $$($(1)_OUT)/port/%.o: $$($(1)_PORT)/%.c $$(KIT_DIR)/kit.h \
		$$(BUILD_PORT)/build_game.h $$($(1)_TREE)/.prepared
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(1)_CFLAGS) -Wall -Wno-unused-parameter -Wno-sign-compare -MMD -MP -c $$< -o $$@

$$(BUILD_PORT_S:%=$$($(1)_OUT)/port/%.o): $$($(1)_OUT)/port/%.o: $$(BUILD_PORT)/%.S
	@mkdir -p $$(dir $$@)
	$$(CC) $$(ARCH_FLAGS) -marm -c $$< -o $$@

$$($(1)_OUT)/port/$(1)_os.o: $$($(1)_PORT)/$(1)_os.c $$(KIT_DIR)/kit_app.h sdk/app.h include/os/app.h
	@mkdir -p $$(dir $$@)
	$$(CC) $$(KIT_OS_CFLAGS) -MMD -MP -c $$< -o $$@

$$(BUILD)/ports/$$($(1)_APP).elf: $$($(1)_OBJS) sdk/app.ld
	$$(CC) $$(KIT_LDFLAGS) -Wl,--wrap=Bgetsysmemsize -Wl,--wrap=__aeabi_ldivmod \
		-Wl,--wrap=drawrooms -Wl,--wrap=drawmasks -Wl,-Map,$$(@:.elf=.map) $$($(1)_OBJS) $$(KIT_LIBS) -o $$@

$$(BUILD)/ports/$$($(1)_APP).app: $$(BUILD)/ports/$$($(1)_APP).elf sdk/mkapp.py
	$$(PYTHON) sdk/mkapp.py $$< $$@

-include $$($(1)_OBJS:.o=.d)
endef

endif
