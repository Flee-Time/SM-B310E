# WavPack: Rockbox's wavpack.c and libwavpack (BSD) with its ARM
# assembly, the WavPack metadata parser (APE tags).
# SPDX-License-Identifier: GPL-2.0-or-later
wavpack_SRCS := codecs/wavpack.c \
                $(addprefix codecs/libwavpack/,bits.c float.c metadata.c unpack.c words.c wputils.c) \
                metadata/wavpack.c metadata/ape.c
wavpack_ASRCS := codecs/libwavpack/arm.S codecs/libwavpack/arml.S
wavpack_CFLAGS := -O1
