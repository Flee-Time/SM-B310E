/* SC6530C audio register facts from unpatched e52q7a stock playback.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SC6530C_AUDIO_TARGET_H
#define SC6530C_AUDIO_TARGET_H

#define SC_AUDIO_REG(a) (*(volatile uint32_t *)(a))
#define SC_VBC_BASE 0x82003000u
#define SC_VBC_CTRL SC_AUDIO_REG(SC_VBC_BASE + 0x18)
#define SC_VBC_FRAMES 160u
#define SC_VBC_CPU_ACCESS (1u << 10)
#define SC_VBC_PLAY ((1u << 15) | (1u << 14) | (1u << 13))
#define SC_CODEC_DP_CTL SC_AUDIO_REG(0x8a00200c)
#define SC_CODEC_MUTE 0xc000u

void audiohw_mute(bool mute);
bool headphones_inserted(void);
void DMA(void);

#endif
