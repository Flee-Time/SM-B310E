# AAC in MP4 (.m4a): Rockbox's aac.c, libfaad (GPLv2+, fixed point,
# with SBR and PS for HE-AAC) and libm4a, the MP4 metadata parser.
# SPDX-License-Identifier: GPL-2.0-or-later
aac_SRCS := codecs/aac.c \
            $(addprefix codecs/libfaad/,bits.c common.c decoder.c drc.c error.c filtbank.c \
              huffman.c is.c mp4.c ms.c pns.c ps_dec.c ps_syntax.c pulse.c sbr_dct.c \
              sbr_dec.c sbr_e_nf.c sbr_fbt.c sbr_hfadj.c sbr_hfgen.c sbr_huff.c sbr_qmf.c \
              sbr_syntax.c sbr_tf_grid.c specrec.c syntax.c tns.c) \
            codecs/libm4a/m4a.c codecs/libm4a/demux.c \
            metadata/mp4.c
aac_CFLAGS := -I$(RBCODEC)/codecs/libfaad
