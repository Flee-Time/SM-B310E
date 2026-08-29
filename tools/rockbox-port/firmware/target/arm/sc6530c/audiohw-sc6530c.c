/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025 by Sho Tanimoto
 * Copyright (C) 2026 by B310E-OS project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "audio.h"
#include "audiohw.h"

/*
 * B310E-OS Rockbox port — b310e/target/arm/sc6530c/audiohw-sc6530c.c
 * (GPLv2, Rockbox-derived; modeled on Rockbox's s3c2440/gigabeat-fx/
 * wmcodec-meg-fx.c and the B310E-OS drivers/audio.c — clean-room register
 * facts, no MOCOR code).
 *
 * The SC6530C AUDIO data path is DSP/VBC-driven; the custom-OS route is
 * the VBC 8 kHz DA path. audiohw_init() brings up the WHOLE chain in the
 * order the emulator captures captured the STOCK bring-up (D10) plus the
 * missing-step fixes from docs/dsp-audio-route.md (D1-D5, D8):
 *
 *   subsystem phase (P2-P6): AHB gate 0x205000c0 |= 0x60; 23-bit APB
 *     ladder on 0x8b0000a0; the D1 ANA clock block 0x820010e0/e4/0x1040
 *     (the #1 missing step); 14-bit AHB ladder on 0x20500060; the
 *     MAILBOX-SAFE ANA pad half of the pinmap 0x82001850..0x1884 (the
 *     0x8c half stays BANNED on HW);
 *   codec ladder (0x820012C0 6 bits + companion GPIOs + 0x82001294=0);
 *   DAC path (r2=1 ONLY — all-three r2 HANGS the phone);
 *   DAC-on chain (ccr clocks -> audif_enb gate -> dacr cores -> routing
 *     -> unmute gains; dcgr 0xF = mute, so this is mandatory);
 *   DP (digital-die) DAC (0x8A002000: FS 8 kHz, unmute, DAC_EN_L/R);
 *   PA enable + unmute (ownership 0x604 = b9|b10|b2 — NOT the old 0x200);
 *   VBC DA data path (D8): vb_ctl bits 5/6/8 + b2 ARM_VB_ACC, IIS sel +
 *     DAPATHCTL routing, buffer size, VBDALDMA_EN/VBDARDMA_EN 1<<13/14 +
 *     VBENABLE 1<<15 on 0x82003018, VBC power half 1<<18 on 0x8b000060/64,
 *     VBDAHP_EN bit10 clear @0x82003048 + STCTL HPF bits @0x82003078/0x7C;
 *   keypad EIC re-assert (the ANA clock block clobbers 0x820010e0/e4 —
 *     the END key must survive).
 *
 * SAFETY: no 0x8c writes; the DAC gains (0x82001314/18/1C) are only
 * touched after the clocks/DAC (chain order); all waits are bounded
 * volatile loops; NEVER write the all-three r2 device configs.
 *
 * VOLUME: the sink's volume_type is PCM_SINK_SWVOL (HAVE_SW_VOLUME_CONTROL)
 * — Rockbox's swvol DSP path (pcm_sw_volume.c) applies the volume to the
 * samples BEFORE ops.play(), so audiohw_set_volume() must NOT also scale
 * the sink output (double-scaling). It records the value for debug.
 */

#define REG32(a) (*(volatile uint32_t *)(a))

/* ---- Bounded ADI mailbox (fpdoom/B310E-OS pattern) --------------------- */
#define ADI_RD_CMD      0x82000018
#define ADI_RD_DATA     0x8200001C
#define ADI_FIFO_STS    0x82000020
#define ADI_FIFO_FULL   (1 << 9)
#define ADI_FIFO_EMPTY  (1 << 8)
#define ADI_BUDGET      1000000u

static uint32_t audio_adi_read(uint32_t addr)
{
    uint32_t a = 0, n = ADI_BUDGET;

    REG32(ADI_RD_CMD) = addr & 0xfff;
    while ((a = REG32(ADI_RD_DATA)) >> 31)
        if (--n == 0) break;
    return a & 0xffffu;
}

static void audio_adi_write(uint32_t addr, uint32_t val)
{
    uint32_t n = ADI_BUDGET;

    while (REG32(ADI_FIFO_STS) & ADI_FIFO_FULL)
        if (--n == 0) return;
    REG32(addr) = val;
    n = ADI_BUDGET;
    while (!(REG32(ADI_FIFO_STS) & ADI_FIFO_EMPTY))
        if (--n == 0) return;
}

static void audio_reg_bit(uint32_t addr, uint32_t bit, int en)
{
    uint32_t v = audio_adi_read(addr);

    if (en) v |= bit; else v &= ~bit;
    audio_adi_write(addr, v);
}

/* Bounded volatile delay (~208 MHz, ~40000 iterations/ms). Never use the
 * free-running counter in the audio path (its stability loop can hang). */
static void audio_busy_ms(uint32_t ms)
{
    volatile uint32_t d = ms * 40000u;

    while (d--)
        ;
}

/* ---- register map (audio.h + dump-mined facts) ------------------------- */
#define AUD_CODEC_BASE      0x82001280u
#define AUD_REG_PA_MUTE     (AUD_CODEC_BASE + 0x10u)  /* 0x82001290 */
#define AUD_REG_PWR         (AUD_CODEC_BASE + 0x40u)  /* 0x820012C0 */
#define AUD_REG_AUDIF_ENB   (AUD_CODEC_BASE + 0x00u)
#define AUD_REG_DACR        (AUD_CODEC_BASE + 0x74u)
#define AUD_REG_DAOCR2      (AUD_CODEC_BASE + 0x7Cu)
#define AUD_REG_DCR1        (AUD_CODEC_BASE + 0x84u)
#define AUD_REG_GAIN_HP     (AUD_CODEC_BASE + 0x94u)  /* 0x82001314 */
#define AUD_REG_GAIN_EAR    (AUD_CODEC_BASE + 0x98u)  /* 0x82001318 */
#define AUD_REG_GAIN_SPK    (AUD_CODEC_BASE + 0x9Cu)  /* 0x8200131C */
#define AUD_REG_CCR         (AUD_CODEC_BASE + 0xACu)  /* 0x8200132C */
#define AUD_REG_IF0         0x82001180u
#define AUD_REG_IF1         0x820011A0u
#define AUD_DAC_PATH_GRP0   0x820012A0u
#define AUD_DAC_PATH_GRP1   0x820012A4u

#define AUD_DP_BASE         0x8A002000u
#define AUD_DP_TOP_CTL      (AUD_DP_BASE + 0x00u)
#define AUD_DP_DAC_CTL      (AUD_DP_BASE + 0x0Cu)
#define AUD_DP_SDM_CTL0     (AUD_DP_BASE + 0x10u)

#define AUD_REG_PA_EN       0x82001450u
#define AUD_REG_PA_EN2      0x82001454u
#define AUD_REG_PA_A        0x82001440u
#define AUD_REG_PA_B        0x82001444u
#define AUD_APB_PA_PWR      0x8B000060u   /* PA bit21 + VBC bit18   */
#define AUD_APB_PA_PWR2     0x8B000064u
#define AUD_APB_PA_PWR_HI   0x8B0000A0u   /* PA bit28               */
#define AUD_APB_PA_PWR2_HI  0x8B0000A4u
#define AUD_APB_PERI_CTL0   0x8B0001C4u   /* PA b9/b10 + vb_ctl     */

#define AUD_PWR_BG          (1u << 1)
#define AUD_PWR_IB          (1u << 3)
#define AUD_PWR_VCM         (1u << 7)
#define AUD_PWR_VCM_BUF     (1u << 6)
#define AUD_PWR_VB          (1u << 5)
#define AUD_PWR_VBO         (1u << 4)

/* VBC 8 kHz DA path (vbc_phy_v5.h + dsp-data-path.md D8). */
#define VBC_BASE        0x82003000u
#define VBC_VBDAL       (VBC_BASE + 0x00u)
#define VBC_VBDAR       (VBC_BASE + 0x04u)
#define VBC_BUFFSIZE    (VBC_BASE + 0x10u)
#define VBC_DABUFFDTA   (VBC_BASE + 0x18u)
#define VBC_IISSEL      (VBC_BASE + 0x3Cu)
#define VBC_DAPATHCTL   (VBC_BASE + 0x40u)
#define VBC_DAHPCTL     (VBC_BASE + 0x48u)
#define VBC_STCTL0      (VBC_BASE + 0x78u)
#define VBC_STCTL1      (VBC_BASE + 0x7Cu)
#define VBC_DA_BUF_SIZE 160u

#define VBC_ENABLE      (1u << 15)  /* VBENABLE                  */
#define VBC_DALDMA_EN   (1u << 13)  /* VBDALDMA_EN (D8)          */
#define VBC_DARDMA_EN   (1u << 14)  /* VBDARDMA_EN (D8)          */
#define VBC_HPF_EN      (1u << 10)  /* VBDAHP_EN @ DAHPCTL       */
#define VBC_STCTL_HPF   (1u << 11)  /* VBST_HPF_EN @ STCTL0/1    */
#define VBC_PWR_HALF    (1u << 18)  /* VBC power half 0x8b000060 */

/* vb_ctl (APB_PERI_CTL0 0x8B0001C4): ANA + DA0 + DA1 clocks + ARM_VB_ACC. */
#define VBC_CTL_ANA     (1u << 8)
#define VBC_CTL_DA0     (1u << 5)
#define VBC_CTL_DA1     (1u << 6)
#define VBC_CTL_ARMACC  (1u << 2)

/* PA-mute ownership: b9 CLK_AUD_ARM_CTRL | b10 AUD_CTRL_SEL | b2 ARM_VB_ACC
 * (the corrected D5 set — NOT the old single 0x200). */
#define PA_OWNERSHIP    0x604u

static int s_audio_initialized;

/* ---- GPIO (fpdoom gpio_set layout — the codec ladder's companions) ----- */
static void audio_gpio_set(unsigned id, unsigned off, int state)
{
    uint32_t addr, tmp, shl = id & 0xfu;

    addr = (id >= 128)
        ? (0x82001580u) + ((id >> 4) << 6)
        : 0x8a000000u + ((id >> 4) << 7);
    addr += off;
    if (id >= 128) tmp = audio_adi_read(addr);
    else           tmp = REG32(addr);
    tmp &= ~(1u << shl);
    tmp |= (uint32_t)state << shl;
    if (id >= 128) audio_adi_write(addr, tmp);
    else           REG32(addr) = tmp;
}

/* ---- subsystem power phase (P2-P6, docs/audio-emulator-findings.md §3) - */
static void audio_subsystem_power(void)
{
    /* P2: AHB power gate. */
    REG32(0x205000c0) |= 0x60u;

    /* P3: 23-bit APB ladder on 0x8b0000a0, in the captured order. The
     * stock wrote each bit absolutely; we OR to never clear bits that the
     * boot chain already set (LCD/keypad/timer share this register). */
    {
        static const uint32_t bits[23] = {
            0x1000000, 0x400000, 0x10000, 0x40000000, 0x2, 0x200000, 0x200,
            0x80000, 0x40, 0x800000, 0x80, 0x4000000, 0x2000000, 0x100,
            0x2000, 0x400, 0x800, 0x10, 0x8000, 0x4000, 0x20000, 0x8,
            0x80000000,
        };
        unsigned i;
        uint32_t v = REG32(0x8b0000a0);

        for (i = 0; i < 23; i++) {
            v |= bits[i];
            REG32(0x8b0000a0) = v;
            audio_busy_ms(1);
        }
    }

    /* P4 / D1: the ANA clock block — the #1 missing step. ABSOLUTE writes
     * in the captured order. NOTE: this clobbers the keypad EIC channels
     * (0x820010e0/e4) — audio_eic_reapply() re-asserts them afterwards. */
    audio_adi_write(0x820010e0, 0x4);
    audio_adi_write(0x820010e4, 0x2);
    audio_adi_write(0x820010e0, 0x10);
    audio_adi_write(0x820010e4, 0x10);
    audio_adi_write(0x820010e0, 0x20);
    audio_adi_write(0x82001040, 0x2);
    audio_adi_write(0x82001040, 0x1);
    audio_adi_write(0x820010e0, 0x8);
    audio_adi_write(0x820010e4, 0x4);
    audio_adi_write(0x820010e0, 0x2);
    audio_adi_write(0x820010e4, 0x8);
    audio_adi_write(0x820010e0, 0x40);
    audio_adi_write(0x820010e4, 0x20);
    audio_adi_write(0x820010e0, 0x80);
    audio_adi_write(0x820010e0, 0x100);

    /* P5: 14-bit AHB ladder on 0x20500060 (the LCD already set bits 6/12 —
     * OR, never clear). */
    {
        static const uint32_t bits[14] = {
            0x4000, 0x2, 0x400, 0x100, 0x1, 0x4, 0x8, 0x800, 0x10000,
            0x8000, 0x20, 0x1000, 0x40, 0x80,
        };
        unsigned i;
        uint32_t v = REG32(0x20500060);

        for (i = 0; i < 14; i++) {
            v |= bits[i];
            REG32(0x20500060) = v;
            audio_busy_ms(1);
        }
    }

    /* P6: ANA pad half of the pinmap (MAILBOX-SAFE — the 0x8c half stays
     * BANNED on HW). Absolute writes in the captured order. */
    audio_adi_write(0x82001874, 0x18a);
    audio_adi_write(0x82001850, 0x18a);
    audio_adi_write(0x82001878, 0x10a);
    audio_adi_write(0x8200187c, 0x100);
    audio_adi_write(0x82001884, 0x100);
    audio_adi_write(0x82001880, 0x101);
}

/* ---- codec power ladder (stock 0x80982 order) --------------------------- */
static void audio_codec_power(void)
{
    audio_reg_bit(AUD_REG_PWR, AUD_PWR_BG, 1);
    audio_reg_bit(AUD_REG_PWR, AUD_PWR_IB, 1);
    audio_reg_bit(AUD_REG_PWR, AUD_PWR_VCM, 1);
    audio_gpio_set(31, 8, 1);
    audio_gpio_set(31, 0, 1);
    audio_reg_bit(AUD_REG_PWR, AUD_PWR_VCM_BUF, 1);
    audio_gpio_set(30, 8, 1);
    audio_gpio_set(30, 0, 1);
    audio_reg_bit(AUD_REG_PWR, AUD_PWR_VB, 1);
    audio_gpio_set(29, 8, 1);
    audio_gpio_set(29, 0, 1);
    audio_reg_bit(AUD_REG_PWR, AUD_PWR_VBO, 1);
    audio_gpio_set(2, 8, 1);
    audio_gpio_set(2, 0, 1);

    audio_adi_write(AUD_CODEC_BASE + 0x14u, 0u);
}

/* ---- DAC output-path enable (r2=1 ONLY — all-three r2 HANGS) ----------- */
static void audio_dac_path_on(void)
{
    uint32_t v;

    v = audio_adi_read(AUD_REG_IF0);
    audio_adi_write(AUD_REG_IF0, v & ~0x10u);
    audio_busy_ms(3);
    v = audio_adi_read(AUD_REG_IF1);
    audio_adi_write(AUD_REG_IF1, v | 0x10u);
    audio_busy_ms(3);
    v = audio_adi_read(AUD_REG_IF0);
    audio_adi_write(AUD_REG_IF0, v & 0x7FFFu);
    audio_busy_ms(3);
    v = audio_adi_read(AUD_REG_IF1);
    audio_adi_write(AUD_REG_IF1, v | 0x8000u);
    audio_busy_ms(3);

    v = audio_adi_read(AUD_DAC_PATH_GRP0);
    audio_adi_write(AUD_DAC_PATH_GRP0, (v & ~0x10u) | 0x20u);
    audio_busy_ms(3);
    v = audio_adi_read(AUD_DAC_PATH_GRP1);
    audio_adi_write(AUD_DAC_PATH_GRP1, (v & ~0x40u) | 0x80u);
    audio_busy_ms(3);
}

/* ---- DAC-on chain (ccr -> audif_enb -> dacr -> routing -> unmute) ------ */
static void audio_dac_on(void)
{
    uint32_t v;

    /* 1. DAC clocks: ccr |= DRV_CLK_EN(b3) | DAC_CLK_EN(b4). bit6 (0x40)
     * is AUD_ADC_CLK_PD — NOT a DAC enable (a stock trap). */
    v = audio_adi_read(AUD_REG_CCR);
    audio_adi_write(AUD_REG_CCR, v | 0x18u);
    audio_busy_ms(3);

    /* 2. Data gate: audif_enb |= DAC_EN_L(b0) | DAC_EN_R(b2). */
    v = audio_adi_read(AUD_REG_AUDIF_ENB);
    audio_adi_write(AUD_REG_AUDIF_ENB, v | 0x05u);
    audio_busy_ms(3);

    /* 3. DAC cores: dacr |= DACL_EN(b7) | DACR_EN(b6). */
    v = audio_adi_read(AUD_REG_DACR);
    audio_adi_write(AUD_REG_DACR, v | 0xC0u);
    audio_busy_ms(3);

    /* 4. Routing: jack (daocr2 bits 7/2 + dcr1 bits 7/6) + speaker (AOL:
     * dcr1 bit4) — the outputs share the DAC. */
    v = audio_adi_read(AUD_REG_DAOCR2);
    audio_adi_write(AUD_REG_DAOCR2, v | 0x84u);
    audio_busy_ms(3);
    v = audio_adi_read(AUD_REG_DCR1);
    audio_adi_write(AUD_REG_DCR1, v | 0xD0u);
    audio_busy_ms(3);

    /* 5. Unmute gains (dcgr1-3): gain 4 (~-24 dB); 0xF = mute. Only here,
     * AFTER the clocks/DAC — never before (HARD SAFETY). */
    audio_adi_write(AUD_REG_GAIN_HP,  0x44u);
    audio_busy_ms(3);
    audio_adi_write(AUD_REG_GAIN_EAR, 0x40u);
    audio_busy_ms(3);
    audio_adi_write(AUD_REG_GAIN_SPK, 0x44u);
    audio_busy_ms(3);
}

/* ---- DP (digital-die) DAC: FS 8 kHz, unmute, enable cores ------------- */
static void audio_dp_dac_on(void)
{
    uint32_t v;

    v = REG32(AUD_DP_SDM_CTL0);
    REG32(AUD_DP_SDM_CTL0) = v;

    /* DAC FS mode = 8000 Hz (VBC rate; mode 9 from the dump's FS table). */
    v = REG32(AUD_DP_DAC_CTL) & ~0xFu;
    REG32(AUD_DP_DAC_CTL) = v | 9u;

    /* Clear the digital mute (DAC_MUTE_EN bit15). */
    v = REG32(AUD_DP_DAC_CTL) & ~(1u << 15);
    REG32(AUD_DP_DAC_CTL) = v;

    /* Enable the DP DAC cores: top_ctl DAC_EN_L(b0) + DAC_EN_R(b2). */
    v = REG32(AUD_DP_TOP_CTL) | 0x05u;
    REG32(AUD_DP_TOP_CTL) = v;
}

/* ---- VBC DA data path (D8) — the custom-OS 8 kHz PCM route ------------- */
static void audio_vbc_da_on(void)
{
    uint32_t v;

    /* vb_ctl = APB_PERI_CTL0: ANA + DA0 + DA1 clocks + ARM_VB_ACC (b2). */
    v = REG32(AUD_APB_PERI_CTL0) | VBC_CTL_ANA | VBC_CTL_DA0 | VBC_CTL_DA1 |
        VBC_CTL_ARMACC;
    REG32(AUD_APB_PERI_CTL0) = v;

    /* Routing the SDK's playback always sets: IIS port NORMAL + no FM mix. */
    v = REG32(VBC_IISSEL) & ~3u;
    REG32(VBC_IISSEL) = v;
    v = REG32(VBC_DAPATHCTL) & ~3u;
    REG32(VBC_DAPATHCTL) = v;

    /* DA buffer size (low 8 bits = size-1). */
    v = REG32(VBC_BUFFSIZE) & ~0xFFu;
    REG32(VBC_BUFFSIZE) = v | (VBC_DA_BUF_SIZE - 1u);

    /* VBC power half 1<<18 on 0x8b000060/64 (bit 18 — NOT the PA's 21).
     * OR into whatever the PA ladder left. */
    REG32(AUD_APB_PA_PWR)  |= VBC_PWR_HALF;
    REG32(AUD_APB_PA_PWR2) |= VBC_PWR_HALF;

    /* VBC DA HPF stage: programmed by NEITHER core (0 ARM literal hits in
     * the dump) — clear it so the reset default cannot gate/attenuate the
     * 8 kHz PCM (the stock DSP would program these; we don't boot it). */
    v = REG32(VBC_DAHPCTL) & ~VBC_HPF_EN;
    REG32(VBC_DAHPCTL) = v;
    v = REG32(VBC_STCTL0) & ~VBC_STCTL_HPF;
    REG32(VBC_STCTL0) = v;
    v = REG32(VBC_STCTL1) & ~VBC_STCTL_HPF;
    REG32(VBC_STCTL1) = v;

    /* Enable the DA data path: VBENABLE + the D8 DMA-enable bits. */
    v = REG32(VBC_DABUFFDTA) | VBC_ENABLE | VBC_DALDMA_EN | VBC_DARDMA_EN;
    REG32(VBC_DABUFFDTA) = v;
}

/* ---- PA (codec PA + NCP2817BFC external amp) ---------------------------- */
static void audio_pa_enable(void)
{
    REG32(AUD_APB_PA_PWR)    |= 0x200000u;
    REG32(AUD_APB_PA_PWR2)   |= 0x200000u;
    audio_adi_write(AUD_REG_PA_EN,  1u);
    audio_adi_write(AUD_REG_PA_EN2, 1u);
    REG32(AUD_APB_PA_PWR_HI)  |= 0x10000000u;
    REG32(AUD_APB_PA_PWR2_HI) |= 0x10000000u;
    audio_adi_write(AUD_REG_PA_A, 4u);
    audio_adi_write(AUD_REG_PA_B, 4u);
}

static void audio_pa_unmute(void)
{
    uint32_t v;

    /* Full ARM ownership 0x604 (b9 CLK_AUD_ARM_CTRL | b10 AUD_CTRL_SEL |
     * b2 ARM_VB_ACC) — the corrected D5 set; the old single-bit 0x200 is
     * insufficient for the sound-data path. */
    v = REG32(AUD_APB_PERI_CTL0) | PA_OWNERSHIP;
    REG32(AUD_APB_PERI_CTL0) = v;

    audio_reg_bit(AUD_REG_PA_MUTE, 1u, 0);
}

/* ---- keypad EIC re-assert (the D1 ANA block clobbers 0x820010e0/e4) ---- */
static void audio_eic_reapply(void)
{
    audio_adi_write(0x820010e4, 0x20);
    audio_adi_write(0x820010e0, 0x80);
    audio_adi_write(0x82001904, audio_adi_read(0x82001904) | (1u << 3));
}

/* ---- Rockbox audiohw API ------------------------------------------------ */

void audiohw_init(void)
{
    if (s_audio_initialized)
        return;

    /* Whole chain, stock bring-up order (D10): subsystem power first, then
     * the codec ladder, DAC path, DAC-on, DP DAC, PA, VBC data path. */
    audio_subsystem_power();
    audio_codec_power();
    audio_dac_path_on();
    audio_dac_on();
    audio_dp_dac_on();
    audio_pa_enable();
    audio_pa_unmute();
    audio_vbc_da_on();
    audio_eic_reapply();

    s_audio_initialized = 1;
}

void audiohw_preinit(void)
{
}

void audiohw_postinit(void)
{
}

void audiohw_close(void)
{
}

void audiohw_set_volume(int val)
{
    /* The volume is applied by Rockbox's swvol DSP layer (PCM_SINK_SWVOL)
     * BEFORE ops.play() — scaling here too would double-scale. Record the
     * centibel value for debug. */
    (void)val;
}

void audiohw_set_frequency(int fsel)
{
    (void)fsel;
    /* Only the 8 kHz VBC rate is used; nothing to program. */
}
