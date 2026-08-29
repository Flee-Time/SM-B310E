/*
 * B310E-OS — drivers/audio.h
 *
 * On-die analog audio codec (SC6530C A-die) + external NCP2817BFC speaker
 * amplifier. All register facts below are DISASSEMBLED from the stock
 * dump_firmware.bin (adi_phy_v5.c, codec_sprd.c, sprd_codec_state_v0.c,
 * audio_hal.c) — the SC6530C A-die layout DIFFERS from the SC6531EFM MOCOR
 * reference; do not "fix" these addresses from the SDK.
 *
 * Board path (docs/Troubleshooting.pdf p.8-1 block diagram):
 *   codec DAC EAR_L/EAR_R -> NCP2817BFC (AMP_EN = product GPIO) -> speaker.
 * So a beep needs BOTH the codec analog path AND the external amp enable.
 *
 * Clean-room: register/bit facts only, no MOCOR code copied.
 */

#ifndef B310E_OS_DRIVERS_AUDIO_H
#define B310E_OS_DRIVERS_AUDIO_H

#include <stdint.h>

/* ---- codec register block base (struct[0] of the stock RAM shadow) ------
 * The stock driver keeps a RAM struct at 0x042354B0 whose field [0] is this
 * base; all analog codec registers are base+offset (verified: +0x10 is the
 * PA-mute reg 0x82001290, +0x20 is 0x820012A0, +0x24 is 0x820012A4, +0x40
 * the power reg 0x820012C0, +0x70 volume 0x820012F0, +0x94/+0x98 the HP/EAR
 * gains 0x82001314/0x82001318 — matches the MOCOR v0 struct offset layout
 * at a DIFFERENT base). */
#define AUD_CODEC_BASE     0x82001280u

#define AUD_REG_PA_MUTE    (AUD_CODEC_BASE + 0x10u)  /* bit0 = PA muted     */
#define AUD_REG_DAC_CFG    (AUD_CODEC_BASE + 0x20u)  /* bits 4/5            */
#define AUD_REG_DAC_CFG2   (AUD_CODEC_BASE + 0x24u)  /* bits 6/7/14         */
#define AUD_REG_PWR        (AUD_CODEC_BASE + 0x40u)  /* 6 power bits        */
#define AUD_REG_VOL        (AUD_CODEC_BASE + 0x70u)  /* volume field 0xF0   */
#define AUD_REG_GAIN_HP    (AUD_CODEC_BASE + 0x94u)  /* dcgr1 (4-bit HP)    */
#define AUD_REG_GAIN_EAR   (AUD_CODEC_BASE + 0x98u)  /* dcgr2 (4-bit EAR)   */

/* DAC-on chain (MOCOR v0 codec, base 0x82001280 — the SC6530C codec block):
 *   audif_enb  @ +0x00  DAC_EN_L bit0, DAC_EN_R bit2   (data gate)
 *   dacr       @ +0x74  DACL_EN bit7, DACR_EN bit6      (DAC cores)
 *   daocr2     @ +0x7C  DAC→SPN/SPP/HPL/HPR routing
 *   dcr1       @ +0x84  AOL_EN bit4 (SPK), HPL_EN b7/HPR_EN b6 (jack)
 *   dcgr1-3    @ +0x94/0x98/0x9C  4-bit gains, 0xF = MUTE (reset default!)
 *   ccr        @ +0xAC  DRV_CLK_EN bit3, DAC_CLK_EN bit4
 * NOTE: ccr bit6 (0x40) is AUD_ADC_CLK_PD — NOT a DAC enable (a trap;
 * the stock "DAC enable" fn 0x80AF4 writes 0x40 = ADC power-down). */
#define AUD_REG_AUDIF_ENB  (AUD_CODEC_BASE + 0x00u)  /* 0x82001280          */
#define AUD_REG_DACR       (AUD_CODEC_BASE + 0x74u)  /* 0x820012F4          */
#define AUD_REG_DAOCR2     (AUD_CODEC_BASE + 0x7Cu)  /* 0x820012FC          */
#define AUD_REG_DCR1       (AUD_CODEC_BASE + 0x84u)  /* 0x82001304          */
#define AUD_REG_GAIN_SPK   (AUD_CODEC_BASE + 0x9Cu)  /* dcgr3 0x8200131C    */
#define AUD_REG_CCR        (AUD_CODEC_BASE + 0xACu)  /* 0x8200132C          */

/* DP (digital-die) codec block @ 0x8A002000 — the SDM/DAC digital side.
 * Confirmed on SC6530C (5 literal refs in dump, incl. codec area 0x081464).
 *   top_ctl   +0x00  DAC_EN_L bit0, DAC_EN_R bit2, DAC_I2S_SEL bits 4-5
 *   aud_clr   +0x04  DAC_PATH_CLR bit0
 *   dac_ctl   +0x0C  DAC_FS_MODE bits 0-3, DAC_MUTE_EN bit15 (digital mute)
 *   sdm_ctl0  +0x10  SDM config (required before DAC enable)
 * The AP-side chain (audif_enb/dacr/dcr1) gates the ANALOG stage; the DP
 * block enables the DIGITAL DAC path + selects the I2S data source. NEVER
 * touched in v1-v9 — the most likely cause of the total silence. */
#define AUD_DP_BASE       0x8A002000u
#define AUD_DP_TOP_CTL    (AUD_DP_BASE + 0x00u)
#define AUD_DP_DAC_CTL    (AUD_DP_BASE + 0x0Cu)
#define AUD_DP_SDM_CTL0   (AUD_DP_BASE + 0x10u)

/* ---- power ladder bits (stock 0x80982, order on) ------------------------ */
#define AUD_PWR_BG         (1u << 1)   /* 0x02 */
#define AUD_PWR_IB         (1u << 3)   /* 0x08 */
#define AUD_PWR_VCM        (1u << 7)   /* 0x80 (gpio 31)  */
#define AUD_PWR_VCM_BUF    (1u << 6)   /* 0x40 (gpio 30)  */
#define AUD_PWR_VB         (1u << 5)   /* 0x20 (gpio 29)  */
#define AUD_PWR_VBO        (1u << 4)   /* 0x10 (gpio 2)   */

/* ---- LDO / DAC mode registers (stock audio_hal 0x06A14C/0x06A2A4) ------ */
#define AUD_REG_LDO        0x82001014u /* b15 on, b14, b2/b3               */
#define AUD_REG_IF0        0x82001180u /* b4/b0                            */
#define AUD_REG_IF1        0x820011A0u /* b4/b15                           */
#define AUD_REG_IF2        0x8200116Cu /* b10                              */
#define AUD_REG_DAC1       0x82001288u /* b10, mask 0xFFFD                 */

/* DAC output-path enable group — disassembled from stock audio_hal
 * 0x06A14C (device LDO/mode) + 0x06A2A4 (device mux). r2 = device
 * (0/1/2). ON: r2=0 -> 0x820012A0 &= ~8, |= 4; r2=1 -> 0x82001180 &= ~0x10,
 * 0x820011A0 |= 0x10, 0x82001180 &= 0x7FFF, 0x820011A0 |= 0x8000; r2=2 ->
 * 0x82001014 &= 0x7FFF, |= 0x4000. Mux: r2=0 -> 0x820012A0 &= ~0x20, |= 0x10;
 * r2=1 -> 0x820012A0 &= ~0x10, |= 0x20 + 0x820012A4 &= ~0x40, |= 0x80;
 * r2=2 -> 0x820012A4 &= ~0x80, |= 0x40. The stock runs these per-device on
 * open; which r2 maps to the B310E speaker is unknown, so the first beep
 * test enables ALL three (belt-and-suspenders). */
#define AUD_DAC_PATH_GRP0  0x820012A0u /* bit4/5/8 (device 0/1)             */
#define AUD_DAC_PATH_GRP1  0x820012A4u /* bit6/7 (device 1/2)               */

/* ---- PA (stock 0x81516/0x81562/0x81470) -------------------------------- */
#define AUD_REG_PA_A       0x82001440u /* write 1/2/4 (headset/speaker)    */
#define AUD_REG_PA_B       0x82001444u
#define AUD_REG_PA_EN      0x82001450u /* write 1                          */
#define AUD_REG_PA_EN2     0x82001454u

/* APB PA power + ARM ownership (GLB block, matches verified B310E hw)     */
#define AUD_APB_PA_PWR     0x8B000060u /* bit 21 (both halves 0x60/0x64)   */
#define AUD_APB_PA_PWR2    0x8B000064u
#define AUD_APB_PA_PWR_HI  0x8B0000A0u /* bit 28 (type-2, both halves)     */
#define AUD_APB_PA_PWR2_HI 0x8B0000A4u
#define AUD_APB_PERI_CTL0  0x8B0001C4u /* bit9 = 0x200 (stock 0x81470)    */

/* External NCP2817BFC AMP_EN GPIO — UNKNOWN, probe first (see plan §6.2).
 * Family candidates (MOCOR gpio_cfg): 18/39. Stock codec power ladder also
 * toggles GPIOs 2, 28, 29, 30, 31 — see aud_pa_gpio_probe. */
#define AUD_AMP_EN_GPIO_UNKNOWN (-1)

/* ---- API ---------------------------------------------------------------- */
int  audio_init(void);                 /* module init: ARM-own + power-on   */
void audio_beep(uint16_t freq_hz, uint16_t ms);  /* tone test (see notes)  */
void audio_pa_mute(int mute);          /* PA mute bit + APB ownership       */
void audio_pa_gpio_set(int on);        /* NCP2817BFC AMP_EN (probe first)  */
int  audio_gpio_probe_next(void);       /* advance AMP_EN candidate, returns id */

/* Bounded-wait budget (cooperative scheduler must never stall). */
#define AUD_ADI_BUDGET 1000000u

#endif /* B310E_OS_DRIVERS_AUDIO_H */
