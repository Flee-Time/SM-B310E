# Opus: Rockbox's opus.c and libopus (BSD, fixed point, with Rockbox's
# ARMv5E assembly), the TLSF allocator, the Ogg metadata parser.
# SPDX-License-Identifier: GPL-2.0-or-later
OPUS_DIR := codecs/libopus
opus_SRCS := codecs/opus.c \
             $(addprefix $(OPUS_DIR)/celt/,bands.c celt.c celt_decoder.c celt_lpc.c cwrs.c \
               entcode.c entdec.c entenc.c kiss_fft.c laplace.c mathops.c mdct.c pfa.c \
               modes.c pitch.c quant_bands.c rate.c vq.c) \
             $(addprefix $(OPUS_DIR)/silk/,bwexpander_32.c bwexpander.c CNG.c code_signs.c \
               dec_API.c decode_core.c decode_frame.c decode_indices.c decode_parameters.c \
               decode_pitch.c decode_pulses.c decoder_set_fs.c gain_quant.c init_decoder.c \
               lin2log.c log2lin.c LPC_analysis_filter.c LPC_fit.c LPC_inv_pred_gain.c \
               NLSF2A.c NLSF_decode.c NLSF_stabilize.c NLSF_unpack.c NLSF_VQ_weights_laroia.c \
               pitch_est_tables.c PLC.c resampler.c resampler_private_AR2.c \
               resampler_private_down_FIR.c resampler_private_IIR_FIR.c \
               resampler_private_up2_HQ.c resampler_rom.c shell_coder.c sort.c \
               stereo_decode_pred.c stereo_MS_to_LR.c sum_sqr_shift.c table_LSF_cos.c \
               tables_gain.c tables_LTP.c tables_NLSF_CB_NB_MB.c tables_NLSF_CB_WB.c \
               tables_other.c tables_pitch_lag.c tables_pulses_per_block.c) \
             $(OPUS_DIR)/opus.c $(OPUS_DIR)/opus_decoder.c $(OPUS_DIR)/opus_header.c \
             $(OPUS_DIR)/ogg/framing.c \
             ../tlsf/src/tlsf.c \
             metadata/ogg.c metadata/vorbis.c metadata/flac.c
opus_ASRCS := $(addprefix $(OPUS_DIR)/celt/arm/,kiss_fft_armv5e_asm.S mdct_armv5e_asm.S \
                pfa_armv5e_asm.S pitch_armv5e_asm.S comb_filter_armv5e_asm.S \
                denorm_armv5e_asm.S exp_rotation1_armv5e_asm.S haar1_armv5e_asm.S \
                normres_armv5e_asm.S deemph_armv5e_asm.S) \
              $(OPUS_DIR)/silk/arm/resampler_armv5e_asm.S
# libopus has its own config.h (it includes the shim's through rbcodecconfig.h)
opus_INCLUDES := -I$(RBCODEC)/$(OPUS_DIR) -I$(RBCODEC)/$(OPUS_DIR)/celt \
                 -I$(RBCODEC)/$(OPUS_DIR)/silk
opus_CFLAGS := -DHAVE_CONFIG_H -I$(ROCKBOX)/lib/tlsf/src
