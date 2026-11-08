# InfoNES (the NES) as a B310E OS app: $(BUILD)/ports/infones.app (included by
# os/Makefile after the kit; `make infones`, and part of `make` once the
# source is there).
#
# The source is upstream InfoNES (Apache-2.0), pinned and unchanged in
# build/infones (python os/ports/sources.py infones), copied to the build
# with the changes prepare.py lists (its sound's bugs, this phone's speed);
# it is C++ without the library (no exceptions, no RTTI, no new). Ours
# (os/ports/infones):
# its system interface on the kit and the app's OS side. With the GPLv2+
# OS, the app is GPLv3 in effect (Apache-2.0 goes with GPLv3).
#
# SPDX-License-Identifier: GPL-2.0-or-later

INFONES_DIR ?= ../build/infones
INFONES_SRC := $(INFONES_DIR)/src
INFONES_OUT := $(BUILD)/ports/infones
INFONES_TREE := $(INFONES_OUT)/src
INFONES_PORT := ports/infones
INFONES_CXX := $(CROSS)g++

# the CPU, the picture and the sound (InfoNES_Mapper.cpp includes the 138
# mappers): ARM at -O2. On the phone 37-42 frames a second, where Thumb at
# -Os, smaller and so kinder to the cache, drew 24.
INFONES_FILES := InfoNES InfoNES_Mapper InfoNES_pAPU K6502
INFONES_OBJS := $(INFONES_FILES:%=$(INFONES_OUT)/%.o) $(INFONES_OUT)/port/infones_sys.o \
	$(INFONES_OUT)/port/infones_os.o $(KIT_OBJS)
INFONES_CXXFLAGS := $(filter-out -std=gnu11,$(KIT_CFLAGS)) -std=gnu++98 -fno-exceptions \
	-fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -I$(INFONES_TREE)

ifneq ($(wildcard $(INFONES_SRC)/InfoNES.cpp),)
all: $(BUILD)/ports/infones.app
endif

infones: $(BUILD)/ports/infones.app
.PHONY: infones

$(INFONES_SRC)/InfoNES.cpp:
	@echo "InfoNES's source is not in $(INFONES_DIR):" >&2
	@echo "run python os/ports/sources.py infones" >&2
	@false

$(INFONES_TREE)/.prepared: $(INFONES_PORT)/prepare.py | $(INFONES_SRC)/InfoNES.cpp
	$(PYTHON) $(INFONES_PORT)/prepare.py $(INFONES_SRC) $(INFONES_TREE)

$(INFONES_FILES:%=$(INFONES_TREE)/%.cpp): $(INFONES_TREE)/.prepared ;

$(INFONES_FILES:%=$(INFONES_OUT)/%.o): $(INFONES_OUT)/%.o: $(INFONES_TREE)/%.cpp \
		$(INFONES_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(INFONES_CXX) $(INFONES_CXXFLAGS) -marm -O2 -w -MMD -MP -c $< -o $@

$(INFONES_OUT)/port/infones_sys.o: $(INFONES_PORT)/infones_sys.cpp $(KIT_DIR)/kit.h \
		$(INFONES_TREE)/.prepared
	@mkdir -p $(dir $@)
	$(INFONES_CXX) $(INFONES_CXXFLAGS) -marm -O2 -Wall -MMD -MP -c $< -o $@

$(INFONES_OUT)/port/infones_os.o: $(INFONES_PORT)/infones_os.c $(KIT_DIR)/kit_app.h sdk/app.h \
		include/os/app.h
	@mkdir -p $(dir $@)
	$(CC) $(KIT_OS_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/ports/infones.elf: $(INFONES_OBJS) sdk/app.ld
	$(CC) $(KIT_LDFLAGS) -Wl,--wrap=_Z17InfoNES_pAPUVsyncv -Wl,-Map,$(@:.elf=.map) \
		$(INFONES_OBJS) $(KIT_LIBS) -o $@

$(BUILD)/ports/infones.app: $(BUILD)/ports/infones.elf sdk/mkapp.py
	$(PYTHON) sdk/mkapp.py $< $@

-include $(INFONES_OBJS:.o=.d)
