# Quake as a B310E OS app: $(BUILD)/ports/quake.app (included by os/Makefile after the
# kit; `make quake`, and part of `make` once the source is there).
#
# The source is id's WinQuake (GPLv2), pinned and unchanged in build/quake (python
# os/ports/sources.py quake), copied to the build with the changes prepare.py lists.
# Ours (os/ports/quake): its system, video, sound and keys on the kit (quake_sys.c),
# the app's OS side (quake_os.c).
#
# SPDX-License-Identifier: GPL-2.0-or-later

QUAKE_DIR ?= ../build/quake/WinQuake
QUAKE_OUT := $(BUILD)/ports/quake
QUAKE_TREE := $(QUAKE_OUT)/src
QUAKE_PORT := ports/quake

# id's software renderer and the game, as its null drivers take them (no x86 assembly)
QUAKE_SRC := cd_null chase cl_demo cl_input cl_main cl_parse cl_tent cmd common console \
	crc cvar d_edge d_fill d_init d_modech d_part d_polyse d_scan d_sky d_sprite d_surf \
	d_vars d_zpoint draw host host_cmd in_null keys mathlib menu model net_loop net_main \
	net_none net_vcr nonintel pr_cmds pr_edict pr_exec r_aclip r_alias r_bsp r_draw \
	r_edge r_efrag r_light r_main r_misc r_part r_sky r_sprite r_surf r_vars sbar screen \
	snd_dma snd_mem snd_mix sv_main sv_move sv_phys sv_user view wad world zone
# the drawing and the mixing: ARM at -O2; the rest Thumb -Os
QUAKE_ARM := d_edge d_polyse d_scan d_sky d_sprite d_surf d_zpoint r_aclip r_alias r_bsp \
	r_draw r_edge r_light r_main r_surf mathlib snd_mix
QUAKE_OBJS := $(QUAKE_SRC:%=$(QUAKE_OUT)/%.o) $(QUAKE_OUT)/port/quake_sys.o \
	$(QUAKE_OUT)/port/quake_os.o $(KIT_OBJS)
# (1996 C: functions it never declares, pointers it mixes)
QUAKE_CFLAGS := $(filter-out -std=gnu11,$(KIT_CFLAGS)) -std=gnu99 -I$(QUAKE_TREE) \
	-Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types \
	-Wno-error=int-conversion -Wno-error=implicit-int -fcommon

ifneq ($(wildcard $(QUAKE_DIR)/host.c),)
all: $(BUILD)/ports/quake.app
endif

quake: $(BUILD)/ports/quake.app
.PHONY: quake

$(QUAKE_DIR)/host.c:
	@echo "Quake's source is not in $(QUAKE_DIR):" >&2
	@echo "run python os/ports/sources.py quake" >&2
	@false

$(QUAKE_TREE)/.prepared: $(QUAKE_PORT)/prepare.py | $(QUAKE_DIR)/host.c
	$(PYTHON) $(QUAKE_PORT)/prepare.py $(QUAKE_DIR) $(QUAKE_TREE)

$(QUAKE_SRC:%=$(QUAKE_TREE)/%.c): $(QUAKE_TREE)/.prepared ;

$(QUAKE_SRC:%=$(QUAKE_OUT)/%.o): $(QUAKE_OUT)/%.o: $(QUAKE_TREE)/%.c $(QUAKE_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(CC) $(QUAKE_CFLAGS) $(if $(filter $(QUAKE_ARM),$*),-marm -O2,-Os) -w -MMD -MP -c $< -o $@

$(QUAKE_OUT)/port/quake_sys.o: $(QUAKE_PORT)/quake_sys.c $(KIT_DIR)/kit.h $(QUAKE_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(CC) $(QUAKE_CFLAGS) -Os -Wall -Wno-unused-parameter -MMD -MP -c $< -o $@

$(QUAKE_OUT)/port/quake_os.o: $(QUAKE_PORT)/quake_os.c $(KIT_DIR)/kit_app.h sdk/app.h \
		include/os/app.h
	@mkdir -p $(dir $@)
	$(CC) $(KIT_OS_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/ports/quake.elf: $(QUAKE_OBJS) sdk/app.ld
	$(CC) $(KIT_LDFLAGS) -Wl,-Map,$(@:.elf=.map) $(QUAKE_OBJS) $(KIT_LIBS) -o $@

$(BUILD)/ports/quake.app: $(BUILD)/ports/quake.elf sdk/mkapp.py
	$(PYTHON) sdk/mkapp.py $< $@

-include $(QUAKE_OBJS:.o=.d)
