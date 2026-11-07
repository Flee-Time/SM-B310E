# MPEG audio layers 1-3: libmad (GPLv2+), Rockbox's mpa.c, the MP3
# metadata parser (ID3v1/v2, Xing/VBRI/LAME).
# SPDX-License-Identifier: GPL-2.0-or-later
mpa_SRCS := codecs/mpa.c codecs/libasf/asf.c \
            codecs/libmad/bit.c codecs/libmad/frame.c codecs/libmad/huffman.c \
            codecs/libmad/layer12.c codecs/libmad/layer3.c codecs/libmad/stream.c \
            codecs/libmad/synth.c \
            metadata/mp3.c metadata/mp3data.c
mpa_ASRCS := codecs/libmad/imdct_l_arm.S codecs/libmad/synth_full_arm.S
mpa_CFLAGS := -I$(RBCODEC)/codecs/libmad -UDEBUG -DNDEBUG -DHAVE_LIMITS_H -DHAVE_ASSERT_H
