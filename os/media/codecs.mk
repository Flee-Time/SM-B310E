# B310E OS — codec modules from Rockbox's decoders (os/codec.h).
#
# Every folder in media/codecs/ is one module, $(BUILD)/codecs/<folder>.cdc:
# its module.c (glue.h: CODEC_MODULE) and the Rockbox sources its codec.mk
# lists, relative to lib/rbcodec of the Rockbox tree in $(ROCKBOX) (the
# pinned clone of scripts/targets/rockbox.sh; nothing there is changed):
#
#   <name>_SRCS    C sources          <name>_ASRCS  assembly
#   <name>_CFLAGS  extra flags (include folders, -O level)
#   <name>_INCLUDES  include folders searched before the shim (a library
#                    with its own config.h)
#
# (../tlsf/src/tlsf.c reaches Rockbox's lib/tlsf, the allocator of the
# Vorbis and Opus decoders.)
#
# All of them see the shim headers in media/rockbox/ first. Built in ARM
# mode at -O2 as Rockbox builds its codecs; no floating point.
#
# SPDX-License-Identifier: GPL-2.0-or-later

ROCKBOX      ?= ../build/rockbox
RBCODEC      := $(ROCKBOX)/lib/rbcodec
CODEC_NAMES  := $(notdir $(patsubst %/,%,$(wildcard media/codecs/*/)))
CODECS       := $(CODEC_NAMES:%=$(BUILD)/codecs/%.cdc)
CODEC_CFLAGS := $(ARCH_FLAGS) -marm -O2 -g -std=gnu99 -ffreestanding -fno-common \
                -DROCKBOX -DCODEC \
                -ffunction-sections -fdata-sections -fstrict-aliasing \
                -fno-unwind-tables -fno-asynchronous-unwind-tables \
                -fno-tree-loop-distribute-patterns \
                -Imedia/rockbox -Imedia/codecs -Iinclude -I$(RBCODEC) \
                -I$(RBCODEC)/codecs -I$(RBCODEC)/codecs/lib -I$(RBCODEC)/metadata
CODEC_LDFLAGS := $(ARCH_FLAGS) -nostdlib -T media/codecs/codec.ld -Wl,-q \
                 -Wl,--gc-sections -Wl,--pic-veneer -Wl,--no-warn-rwx-segments
# in every module: Rockbox's codeclib (codec_malloc, the string calls
# through codec_api, the MDCT and FFT) and the metadata helpers (ID3 tags
# too); --gc-sections drops what a module does not use
CODEC_COMMON := codecs/lib/codeclib.c codecs/lib/mdct.c codecs/lib/fft-ffmpeg.c                 codecs/lib/mdct_lookup.c metadata/metadata_common.c metadata/id3tags.c

ifneq ($(wildcard $(RBCODEC)/codecs/codecs.h),)
all: $(CODECS)
else
$(warning no Rockbox tree in $(ROCKBOX): the codec modules are not built)
endif

-include $(wildcard media/codecs/*/codec.mk)

# $(1): the module name
define CODEC_template
$(1)_OBJS := $$(addprefix $(BUILD)/codecs/$(1)/rb/,$$(patsubst %.c,%.o,$$($(1)_SRCS) $(CODEC_COMMON))) \
             $$(addprefix $(BUILD)/codecs/$(1)/rb/,$$(patsubst %.S,%.o,$$($(1)_ASRCS))) \
             $(BUILD)/codecs/$(1)/module.o $(BUILD)/codecs/$(1)/glue.o $(BUILD)/codecs/$(1)/rt.o \
             $(BUILD)/codecs/$(1)/setjmp.o

$(BUILD)/codecs/$(1)/rb/%.o: $(RBCODEC)/%.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(1)_INCLUDES) $(CODEC_CFLAGS) $$($(1)_CFLAGS) -w -MMD -MP -c $$< -o $$@

$(BUILD)/codecs/$(1)/rb/%.o: $(RBCODEC)/%.S
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(1)_INCLUDES) $(CODEC_CFLAGS) $$($(1)_CFLAGS) -MMD -MP -c $$< -o $$@

$(BUILD)/codecs/$(1)/module.o: media/codecs/$(1)/module.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(1)_INCLUDES) $(CODEC_CFLAGS) $$($(1)_CFLAGS) -Wall -MMD -MP -c $$< -o $$@

$(BUILD)/codecs/$(1)/%.o: media/codecs/%.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$($(1)_INCLUDES) $(CODEC_CFLAGS) $$($(1)_CFLAGS) -Wall -Wno-unused-parameter -MMD -MP -c $$< -o $$@

$(BUILD)/codecs/$(1)/%.o: media/codecs/%.S
	@mkdir -p $$(dir $$@)
	$$(CC) $(CODEC_CFLAGS) -MMD -MP -c $$< -o $$@

$(BUILD)/codecs/$(1).elf: $$($(1)_OBJS) media/codecs/codec.ld
	$$(CC) $(CODEC_LDFLAGS) -Wl,-Map,$(BUILD)/codecs/$(1).map $$($(1)_OBJS) -lgcc -o $$@

$(BUILD)/codecs/$(1).cdc: $(BUILD)/codecs/$(1).elf sdk/mkapp.py
	$(PYTHON) sdk/mkapp.py --magic 0x44433342 --info codec_module $$< $$@

-include $$($(1)_OBJS:.o=.d)
endef

$(foreach c,$(CODEC_NAMES),$(eval $(call CODEC_template,$(c))))
.PRECIOUS: $(BUILD)/codecs/%.elf
