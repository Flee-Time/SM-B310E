# Apple Lossless in MP4 (.m4a): Rockbox's alac.c, libalac and libm4a,
# the MP4 metadata parser.
# SPDX-License-Identifier: GPL-2.0-or-later
alac_SRCS := codecs/alac.c codecs/libalac/alac.c \
             codecs/libm4a/m4a.c codecs/libm4a/demux.c \
             metadata/mp4.c
alac_CFLAGS :=
