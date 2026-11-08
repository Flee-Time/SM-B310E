# Chocolate Doom, Heretic and Hexen as B310E OS apps: $(BUILD)/ports/doom.app,
# heretic.app, hexen.app (included by os/Makefile after the kit; `make
# chocolate`, and part of `make` once the source is there).
#
# The source is upstream Chocolate Doom, pinned and unchanged in
# build/chocolate-doom (python os/ports/sources.py chocolate-doom). The
# game files build as they are; ours (os/ports/chocolate) are main, the
# video, keys and timer, the sound on the kit's mixer, the stubs for what
# the phone does not have, a few SDL names, and each app's OS side.
#
# SPDX-License-Identifier: GPL-2.0-or-later

CHOCO_DIR ?= ../build/chocolate-doom
CHOCO_SRC := $(CHOCO_DIR)/src
CHOCO_OUT := $(BUILD)/ports/chocolate
CHOCO_PORT := ports/chocolate

# upstream's CMakeLists: the common files (less SDL's, the network's, the
# setup's, the music devices' and the 640 x 480 start-up), the dehacked
# ones. The network's entry points are stubs (stubs.c): its server alone
# would keep 0.5 MB of buffers.
CHOCO_COMMON := i_system m_argv m_misc d_event d_iwad d_loop d_mode deh_str \
	gusconf m_bbox m_cheat m_config m_controls m_fixed sha1 memio tables \
	v_diskicon v_video w_checksum w_main w_wad w_file w_file_stdc w_merge z_zone
CHOCO_DEH := deh_io deh_main deh_mapping deh_text
CHOCO_OURS := i_main i_video i_sound stubs low_detail

# low_detail.c: Doom's low detail in one write a pixel; Heretic's and Hexen's made
# to work (their sky: prepare.py)
CHOCO_WRAP_doom := R_DrawColumnLow R_DrawSpanLow
CHOCO_WRAP_heretic := $(CHOCO_WRAP_doom) R_Init R_DrawTLColumn R_DrawTranslatedColumn \
	R_DrawTranslatedTLColumn M_SaveDefaults
CHOCO_WRAP_hexen := $(CHOCO_WRAP_heretic) R_DrawAltTLColumn R_DrawTranslatedAltTLColumn

CHOCO_GAME_doom := $(notdir $(basename $(filter-out %/doom_icon.c,$(wildcard $(CHOCO_SRC)/doom/*.c))))
CHOCO_GAME_heretic := $(notdir $(basename $(filter-out %/heretic_icon.c,$(wildcard $(CHOCO_SRC)/heretic/*.c))))
CHOCO_GAME_hexen := $(notdir $(basename $(filter-out %/hexen_icon.c,$(wildcard $(CHOCO_SRC)/hexen/*.c))))
CHOCO_DEFS_doom := -DGAME_DOOM
CHOCO_DEFS_heretic := -DGAME_HERETIC
CHOCO_DEFS_hexen := -DGAME_HEXEN

CHOCO_CFLAGS := $(KIT_CFLAGS) -std=gnu99 -I$(CHOCO_PORT) -I$(CHOCO_PORT)/sdl -I$(CHOCO_SRC) \
	-I$(CHOCO_DIR)/textscreen
CHOCO_LDFLAGS := $(KIT_LDFLAGS) -Wl,--wrap=I_ZoneBase -Wl,--wrap=FixedDiv

# Where the time goes is ARM code at -O2 (one instruction for an indexed
# byte load, smull for FixedMul; Thumb on the ARM926 has neither): the
# renderer, the fixed-point maths, the screen's patches, the zone and the
# lumps, the map's geometry and sight lines, the monsters' thinking. The
# rest is Thumb at -Os: smaller, and it runs now and then.
CHOCO_ARM_COMMON := m_fixed v_video w_wad z_zone
CHOCO_ARM_GAME := r_% p_map p_maputl p_sight p_enemy
choco_mode = $(if $(filter $(1),$(2)),-marm -O2,-Os)

# the objects of a game: $(call choco_objs,doom)
choco_objs = $(CHOCO_COMMON:%=$(CHOCO_OUT)/$(1)/common/%.o) \
	$(if $(filter hexen,$(1)),,$(CHOCO_DEH:%=$(CHOCO_OUT)/$(1)/common/%.o)) \
	$(CHOCO_GAME_$(1):%=$(CHOCO_OUT)/$(1)/game/%.o) \
	$(CHOCO_OURS:%=$(CHOCO_OUT)/$(1)/port/%.o) \
	$(CHOCO_OUT)/$(1)/port/chocolate_os.o $(CHOCO_OUT)/$(1)/port/fixed.o $(KIT_OBJS)

CHOCO_GAMES := doom heretic hexen
CHOCO_APPS := $(CHOCO_GAMES:%=$(BUILD)/ports/%.app)

ifneq ($(wildcard $(CHOCO_SRC)/doom/d_main.c),)
all: $(CHOCO_APPS)
endif

chocolate: $(CHOCO_APPS)
.PHONY: chocolate

$(CHOCO_SRC)/doom/d_main.c:
	@echo "Chocolate Doom's source is not in $(CHOCO_DIR):" >&2
	@echo "run python os/ports/sources.py chocolate-doom" >&2
	@false

define CHOCO_GAME_RULES
$(CHOCO_OUT)/$(1)/common/%.o: $(CHOCO_SRC)/%.c | $(CHOCO_SRC)/doom/d_main.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CHOCO_CFLAGS) $$(call choco_mode,$$*,$(CHOCO_ARM_COMMON)) $$(CHOCO_DEFS_$(1)) \
		-I$(CHOCO_SRC)/$(1) -w -MMD -MP -c $$< -o $$@

$(CHOCO_OUT)/$(1)/game/%.o: $(CHOCO_SRC)/$(1)/%.c | $(CHOCO_SRC)/doom/d_main.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CHOCO_CFLAGS) $$(call choco_mode,$$*,$(CHOCO_ARM_GAME)) $$(CHOCO_DEFS_$(1)) \
		-I$(CHOCO_SRC)/$(1) -w -MMD -MP -c $$< -o $$@

$(CHOCO_OUT)/$(1)/port/low_detail.o: $(CHOCO_PORT)/low_detail.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CHOCO_CFLAGS) -marm -O2 $$(CHOCO_DEFS_$(1)) -Wall -MMD -MP -c $$< -o $$@

$(CHOCO_OUT)/$(1)/port/fixed.o: $(CHOCO_PORT)/fixed.S
	@mkdir -p $$(dir $$@)
	$$(CC) $$(ARCH_FLAGS) -marm -c $$< -o $$@

$(CHOCO_OUT)/$(1)/port/%.o: $(CHOCO_PORT)/%.c | $(CHOCO_SRC)/doom/d_main.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CHOCO_CFLAGS) $$(CHOCO_DEFS_$(1)) -I$(CHOCO_SRC)/$(1) -Wall \
		-Wno-unused-parameter -Wno-sign-compare -MMD -MP -c $$< -o $$@

# the OS side: the SDK's app.h (its names are the API's)
$(CHOCO_OUT)/$(1)/port/chocolate_os.o: $(CHOCO_PORT)/chocolate_os.c $(KIT_DIR)/kit_app.h \
		sdk/app.h include/os/app.h
	@mkdir -p $$(dir $$@)
	$$(CC) $$(KIT_OS_CFLAGS) $$(CHOCO_DEFS_$(1)) -MMD -MP -c $$< -o $$@

$(BUILD)/ports/$(1).elf: $$(call choco_objs,$(1)) sdk/app.ld
	$$(CC) $$(CHOCO_LDFLAGS) $$(CHOCO_WRAP_$(1):%=-Wl,--wrap=%) -Wl,-Map,$$(@:.elf=.map) \
		$$(call choco_objs,$(1)) $$(KIT_LIBS) -o $$@

$(BUILD)/ports/$(1).app: $(BUILD)/ports/$(1).elf sdk/mkapp.py
	$$(PYTHON) sdk/mkapp.py $$< $$@

-include $$(patsubst %.o,%.d,$$(call choco_objs,$(1)))
endef

$(foreach g,$(CHOCO_GAMES),$(eval $(call CHOCO_GAME_RULES,$(g))))

# Heretic's and Hexen's r_plane.c, for their sky in low detail
define CHOCO_PLANE_RULES
$(CHOCO_OUT)/$(1)/r_plane.c: $(CHOCO_SRC)/$(1)/r_plane.c $(CHOCO_PORT)/prepare.py
	$$(PYTHON) $(CHOCO_PORT)/prepare.py $$< $$@

$(CHOCO_OUT)/$(1)/game/r_plane.o: $(CHOCO_OUT)/$(1)/r_plane.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CHOCO_CFLAGS) -marm -O2 $$(CHOCO_DEFS_$(1)) -I$(CHOCO_SRC)/$(1) -w -MMD -MP -c $$< -o $$@
endef
$(foreach g,heretic hexen,$(eval $(call CHOCO_PLANE_RULES,$(g))))
