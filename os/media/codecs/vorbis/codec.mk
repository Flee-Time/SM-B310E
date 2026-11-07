# Ogg Vorbis: Rockbox's vorbis.c and libtremor (BSD, the integer Tremor),
# the TLSF allocator, the Ogg metadata parser with Vorbis comments.
# SPDX-License-Identifier: GPL-2.0-or-later
vorbis_SRCS := codecs/vorbis.c \
               $(addprefix codecs/libtremor/,bitwise.c block.c codebook.c floor0.c floor1.c \
                 framing.c info.c mapping0.c registry.c res012.c sharedbook.c synthesis.c \
                 vorbisfile.c window.c oggmalloc.c) \
               ../tlsf/src/tlsf.c \
               metadata/ogg.c metadata/vorbis.c metadata/flac.c
vorbis_CFLAGS := -I$(RBCODEC)/codecs/libtremor -I$(ROCKBOX)/lib/tlsf/src
