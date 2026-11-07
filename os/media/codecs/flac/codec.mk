# FLAC: Rockbox's flac.c and libffmpegFLAC (LGPL 2.1+), the FLAC
# metadata parser with Vorbis comments.
# SPDX-License-Identifier: GPL-2.0-or-later
flac_SRCS := codecs/flac.c codecs/libffmpegFLAC/decoder.c \
             metadata/flac.c metadata/vorbis.c
flac_ASRCS := codecs/libffmpegFLAC/arm.S
flac_CFLAGS := -I$(RBCODEC)/codecs/libffmpegFLAC
