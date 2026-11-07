# WAV: Rockbox's wav.c and libpcm (PCM 8-32 bit, float, G.711, IMA/MS
# and other ADPCMs), the RIFF metadata parser (LIST INFO tags).
# SPDX-License-Identifier: GPL-2.0-or-later
wav_SRCS := codecs/wav.c \
            $(addprefix codecs/libpcm/,linear_pcm.c itut_g711.c dvi_adpcm.c ieee_float.c \
              adpcm_seek.c dialogic_oki_adpcm.c ms_adpcm.c yamaha_adpcm.c \
              ima_adpcm_common.c qt_ima_adpcm.c swf_adpcm.c) \
            metadata/wave.c
wav_CFLAGS := -O1
