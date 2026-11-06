/* B310E OS — shared between the audio drivers (audio.c, pcm.c, fm.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DRIVERS_AUDIO_INTERNAL_H
#define DRIVERS_AUDIO_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

/* Voice band codec (VBC) FIFO block and the digital codec, MMIO */
#define VBC_BASE        0x82003000u
#define VBC_CTRL        REG32(VBC_BASE + 0x18)
#define VBC_FRAMES      160u
#define VBC_CPU_ACCESS  (1u << 10)
#define VBC_PLAY        ((1u << 15) | (1u << 14) | (1u << 13))
#define VBC_DP_CTL      REG32(0x8a00200c)
#define CODEC_MUTE      0xc000u

/* pcm.c */
void pcm_set_attenuation(int db10);     /* 0 or below, -1000 mutes */
bool pcm_busy(void);                    /* playing or recording */

/* fm.c */
void fm_set_volume_db10(int db10);

#endif
