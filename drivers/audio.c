/*
 * B310E-OS — drivers/audio.c
 *
 * On-die SC6530C analog audio codec + external NCP2817BFC speaker amp.
 *
 * Register facts DISASSEMBLED from dump_firmware.bin (2026-08-22):
 *   - ADI helper: adi_phy_v5.c @0x302B6 read / 0x3034E write
 *   - codec power RAM struct @0x042354B0, field[0] = codec base 0x82001280
 *   - power ladder 0x80982 (6 bits on base+0x40, order 0x02,0x08,0x80,
 *     0x40,0x20,0x10; GPIOs 31/30/29/2 accompany the top four)
 *   - PA mute 0x81470: bit0 of 0x82001290 + APB_PERI_CTL0 bits 9-10
 *   - PA enable 0x81516/0x81562: 0x82001450/54, 0x82001440/44,
 *     0x8B000060/64 (bit21), 0x8B0000A0/A4 (bit28)
 *   - LDO/mode config 0x06A14C/0x06A2A4/0x06A35A (see header)
 *
 * The MOCOR v0 struct offset layout (audif_enb@0x00, pmur1@0x40, dacr@0x74,
 * daocr@0x78-0x80, dcr1@0x84, dcgr1-3@0x94-0x9C, ccr@0xAC) is the semantic
 * reference; the SC6530C base is 0x82001280 (NOT the SC6531EFM 0x82001380).
 *
 * First-integration beep = square wave via DAC-gain toggling: proves the
 * WHOLE analog chain (codec DAC -> NCP2817 amp -> speaker) on hardware
 * without needing the VBC PCM/DMA data path yet.
 *
 * Clean-room: register/bit facts only, no MOCOR code copied.
 */

#include "os.h"
#include "../arch/chip.h"
#include "audio.h"
#include "../kernel/module.h"

#ifndef HOST_TEST
/* ---- ADI mailbox (same protocol as led.c/battery.c/rtc.c) --------------- */
#define AUD_ADI_RD_CMD    0x82000018u
#define AUD_ADI_RD_DATA   0x8200001cu
#define AUD_ADI_FIFO_STS  0x82000020u
#define AUD_ADI_FIFO_FULL  (1u << 9)
#define AUD_ADI_FIFO_EMPTY (1u << 8)

static uint32_t aud_adi_read(uint32_t addr)
{
    uint32_t a = 0, n = AUD_ADI_BUDGET;

    MEM4(AUD_ADI_RD_CMD) = addr & 0xfffu;
    while ((a = MEM4(AUD_ADI_RD_DATA)) >> 31)   /* wait busy clear */
        if (--n == 0) break;
    return a & 0xffffu;
}

static void aud_adi_write(uint32_t addr, uint32_t val)
{
    uint32_t n = AUD_ADI_BUDGET;

    while (MEM4(AUD_ADI_FIFO_STS) & AUD_ADI_FIFO_FULL)  /* FIFO full  */
        if (--n == 0) return;
    MEM4(addr) = val;
    n = AUD_ADI_BUDGET;
    while (!(MEM4(AUD_ADI_FIFO_STS) & AUD_ADI_FIFO_EMPTY))
        if (--n == 0) return;
}

/* RMW on an ANA codec register (stock helper 0x80744: en ? set : clear). */
static void aud_reg_bit(uint32_t addr, uint32_t bit, int en)
{
    uint32_t v = aud_adi_read(addr);

    if (en) v |= bit; else v &= ~bit;
    aud_adi_write(addr, v);
}

static void aud_delay_ms(uint32_t ms);

/* ---- GPIO (fpdoom gpio_set layout, same as led.c) ----------------------- */
static void aud_gpio_set(unsigned id, unsigned off, int state)
{
    uint32_t addr, tmp, shl = id & 0xfu;

    addr = (id >= 128)
        ? (0x82001580u) + ((id >> 4) << 6)
        : 0x8a000000u + ((id >> 4) << 7);
    addr += off;
    if (id >= 128) tmp = aud_adi_read(addr);
    else           tmp = MEM4(addr);
    tmp &= ~(1u << shl);
    tmp |= (uint32_t)state << shl;
    if (id >= 128) aud_adi_write(addr, tmp);
    else           MEM4(addr) = tmp;
}
#endif /* !HOST_TEST */

static int s_audio_powered;   /* codec analog rails are up */
static int s_audio_pa_muted;  /* PA mute state */

/* ---- codec power ladder (stock 0x80982 order) --------------------------- */

static void aud_power_bit(uint32_t bit, int en)
{
#if !defined(HOST_TEST)
    aud_reg_bit(AUD_REG_PWR, bit, en);
#endif
}

void audio_power_on(void)
{
#if !defined(HOST_TEST)
    if (s_audio_powered)
        return;

    /* Power rails in stock order: 0x02 -> 0x08 -> 0x80 -> 0x40 -> 0x20 ->
     * 0x10. The top four carry companion GPIOs in the stock code (31, 30,
     * 29, 2) — kept separate so they can be dropped if they prove wrong. */
    aud_power_bit(AUD_PWR_BG, 1);
    aud_power_bit(AUD_PWR_IB, 1);
    aud_power_bit(AUD_PWR_VCM, 1);
    aud_gpio_set(31, 8, 1);   /* DIR output */
    aud_gpio_set(31, 0, 1);   /* high */
    aud_power_bit(AUD_PWR_VCM_BUF, 1);
    aud_gpio_set(30, 8, 1);
    aud_gpio_set(30, 0, 1);
    aud_power_bit(AUD_PWR_VB, 1);
    aud_gpio_set(29, 8, 1);
    aud_gpio_set(29, 0, 1);
    aud_power_bit(AUD_PWR_VBO, 1);
    aud_gpio_set(2, 8, 1);
    aud_gpio_set(2, 0, 1);

    /* struct[0x14] = 0 (stock 0x809C0: base+0x14 written 0 at power-on). */
    aud_adi_write(AUD_CODEC_BASE + 0x14u, 0u);

    s_audio_powered = 1;
#endif
}

void audio_power_off(void)
{
#if !defined(HOST_TEST)
    if (!s_audio_powered)
        return;

    /* Reverse order (stock 0x809D2-0x809FA), delay(2) between. */
    aud_power_bit(AUD_PWR_VBO, 0);
    aud_gpio_set(2, 0, 0);
    aud_power_bit(AUD_PWR_VB, 0);
    aud_gpio_set(29, 0, 0);
    aud_power_bit(AUD_PWR_VCM_BUF, 0);
    aud_gpio_set(30, 0, 0);
    aud_power_bit(AUD_PWR_VCM, 0);
    aud_gpio_set(31, 0, 0);
    aud_power_bit(AUD_PWR_IB, 0);
    aud_power_bit(AUD_PWR_BG, 0);

    s_audio_powered = 0;
#endif
}

/* ---- DAC output-path enable (stock audio_hal 0x06A14C + 0x06A2A4) ------ */

static void aud_dac_path_on(void)
{
#if !defined(HOST_TEST)
    uint32_t v;

    /* 0x06A14C device-ON, r2=1 path only (the DAC/HP output config — the
     * B310E speaker + jack share the codec EAR_L/R DAC outputs). The stock
     * opens ONE device per state-machine step; enabling all three r2 paths
     * at once (as v3/v4 did) HUNG the phone. delay(3) after each write
     * mirrors stock 0x6a3e2 (3 ms, counter-based). */
    v = aud_adi_read(AUD_REG_IF0);
    aud_adi_write(AUD_REG_IF0, v & ~0x10u);
    aud_delay_ms(3);
    v = aud_adi_read(AUD_REG_IF1);
    aud_adi_write(AUD_REG_IF1, v | 0x10u);
    aud_delay_ms(3);
    v = aud_adi_read(AUD_REG_IF0);
    aud_adi_write(AUD_REG_IF0, v & 0x7FFFu);
    aud_delay_ms(3);
    v = aud_adi_read(AUD_REG_IF1);
    aud_adi_write(AUD_REG_IF1, v | 0x8000u);
    aud_delay_ms(3);

    /* 0x06A2A4 device-mux, r2=1: 0x820012A0 &= ~0x10, |= 0x20 +
     * 0x820012A4 &= ~0x40, |= 0x80. */
    v = aud_adi_read(AUD_DAC_PATH_GRP0);
    aud_adi_write(AUD_DAC_PATH_GRP0, (v & ~0x10u) | 0x20u);
    aud_delay_ms(3);
    v = aud_adi_read(AUD_DAC_PATH_GRP1);
    aud_adi_write(AUD_DAC_PATH_GRP1, (v & ~0x40u) | 0x80u);
    aud_delay_ms(3);
#endif
}

/* DP (digital-die) DAC enable — the SDM/DAC digital side at 0x8A002000
 * (sprd_codec_dp_phy_v0.c): SDM config, DAC_EN_L/R, FS mode, clear the
 * digital mute. This is the subsystem NEVER touched in v1-v9 and the prime
 * suspect for the total silence: without the DP DAC enabled + unmuted, the
 * analog chain has no digital input to convert. */
static void audio_dp_dac_on(void)
{
#if !defined(HOST_TEST)
    uint32_t v;

    /* SDM config (__sprd_codec_dp_sdm_set equivalent): sdm_ctl0 fields. */
    v = MEM4(AUD_DP_SDM_CTL0);
    MEM4(AUD_DP_SDM_CTL0) = v;            /* keep current, no change yet */

    /* DAC FS mode = 8000 Hz (VBC rate). Values from the dump's FS table
     * (0x8123E: threshold compares, mode 9 for 8 kHz). */
    v = MEM4(AUD_DP_DAC_CTL) & ~0xFu;
    MEM4(AUD_DP_DAC_CTL) = v | 9u;

    /* Clear the digital mute (DAC_MUTE_EN bit15). */
    v = MEM4(AUD_DP_DAC_CTL) & ~(1u << 15);
    MEM4(AUD_DP_DAC_CTL) = v;

    /* Enable the DP DAC cores: top_ctl DAC_EN_L (bit0) + DAC_EN_R (bit2). */
    v = MEM4(AUD_DP_TOP_CTL) | 0x05u;
    MEM4(AUD_DP_TOP_CTL) = v;
#endif
}

/* DAC output-stage enable — the chain the v5 beep was missing (Oracle
 * synthesis 2026-08-22, MOCOR v0 codec layout at base 0x82001280):
 *   ccr clocks -> audif_enb (data gate) -> dacr (cores) -> routing
 *   (daocr2/dcr1) -> unmute gains. The reset default of dcgr1-3 is 0xF
 *   = MUTE, so without this the DAC is silent even when powered. */
static void audio_dac_on(void)
{
#if !defined(HOST_TEST)
    uint32_t v;

    /* 1. DAC clocks: ccr |= DRV_CLK_EN(bit3) | DAC_CLK_EN(bit4). NOTE ccr
     *    bit6 (0x40) is AUD_ADC_CLK_PD — do NOT confuse it with a DAC
     *    enable (the stock 0x80AF4 "DAC enable" writes 0x40 = ADC off). */
    v = aud_adi_read(AUD_REG_CCR);
    aud_adi_write(AUD_REG_CCR, v | 0x18u);
    aud_delay_ms(3);

    /* 2. Data gate: audif_enb |= DAC_EN_L(bit0) | DAC_EN_R(bit2). */
    v = aud_adi_read(AUD_REG_AUDIF_ENB);
    aud_adi_write(AUD_REG_AUDIF_ENB, v | 0x05u);
    aud_delay_ms(3);

    /* 3. DAC cores: dacr |= DACL_EN(bit7) | DACR_EN(bit6). */
    v = aud_adi_read(AUD_REG_DACR);
    aud_adi_write(AUD_REG_DACR, v | 0xC0u);
    aud_delay_ms(3);

    /* 4. Routing: jack (HPL/HPR: daocr2 bits 7/2 + dcr1 bits 7/6) and
     *    speaker (AOL: dcr1 bit4). Both, since the outputs share the DAC. */
    v = aud_adi_read(AUD_REG_DAOCR2);
    aud_adi_write(AUD_REG_DAOCR2, v | 0x84u);
    aud_delay_ms(3);
    v = aud_adi_read(AUD_REG_DCR1);
    aud_adi_write(AUD_REG_DCR1, v | 0xD0u);
    aud_delay_ms(3);

    /* 5. Unmute gains (dcgr1-3): gain 4 (~-24 dB). 0xF = mute. */
    aud_adi_write(AUD_REG_GAIN_HP,  0x44u);
    aud_delay_ms(3);
    aud_adi_write(AUD_REG_GAIN_EAR, 0x40u);
    aud_delay_ms(3);
    aud_adi_write(AUD_REG_GAIN_SPK, 0x44u);
    aud_delay_ms(3);
#endif
}

/* Counter-based ms delay (stock 0x6a3e2 waits ~3 ms between config writes;
 * here via the 1 ms tick so it stays bounded and scheduler-friendly). */
static void aud_delay_ms(uint32_t ms)
{
#if !defined(HOST_TEST)
    uint32_t start = sched_ticks();
    while (sched_ticks() - start < ms)
        task_yield();
#else
    (void)ms;
#endif
}

/* ---- PA control (stock 0x81470 / 0x81516 / 0x81562) --------------------- */

void audio_pa_mute(int mute)
{
#if !defined(HOST_TEST)
    uint32_t v;

    s_audio_pa_muted = mute;

    /* Stock PA mute (0x81470, decoded verbatim): APB_PERI_CTL0 0x8B0001C0+4
     * = 0x8B0001C4, bit 9 ONLY (0x200) — NOT bits 9-10. Mute = clear bit9
     * + set 0x82001290 bit0; unmute = set bit9 + clear bit0. */
    v = MEM4(AUD_APB_PERI_CTL0);
    if (mute) v &= ~0x200u; else v |= 0x200u;
    MEM4(AUD_APB_PERI_CTL0) = v;

    aud_reg_bit(AUD_REG_PA_MUTE, 1u, mute);
#endif
}

void audio_pa_enable(void)
{
#if !defined(HOST_TEST)
    /* Stock PA enable (0x81516/0x81562): APB power halves + ANA enables. */
    MEM4(AUD_APB_PA_PWR)    = 0x200000u;
    MEM4(AUD_APB_PA_PWR2)   = 0x200000u;
    aud_adi_write(AUD_REG_PA_EN,  1u);
    aud_adi_write(AUD_REG_PA_EN2, 1u);
    MEM4(AUD_APB_PA_PWR_HI)  = 0x10000000u;
    MEM4(AUD_APB_PA_PWR2_HI) = 0x10000000u;
    aud_adi_write(AUD_REG_PA_A, 4u);
    aud_adi_write(AUD_REG_PA_B, 4u);
#endif
}

/* ---- NCP2817BFC AMP_EN GPIO (probe until the right pin is known) -------- */

void audio_pa_gpio_set(int on)
{
#if !defined(HOST_TEST)
    /* AMP_EN is a product GPIO; the number is board-specific and still
     * UNKNOWN. Until audio_gpio_probe_next() identifies it, this is a
     * no-op placeholder. Family candidates: 18, 39 (MOCOR gpio_cfg). */
    (void)on;
#endif
}

int audio_gpio_probe_next(void)
{
#if !defined(HOST_TEST)
    static const unsigned s_candidates[] = { 18u, 39u, 31u, 30u, 29u, 2u };
    static int s_idx = -1;

    if (s_idx >= 0)
        aud_gpio_set(s_candidates[(unsigned)s_idx], 0, 0);  /* release */

    s_idx++;
    if (s_idx >= (int)(sizeof(s_candidates) / sizeof(s_candidates[0])))
        s_idx = 0;

    aud_gpio_set(s_candidates[(unsigned)s_idx], 4, 1);   /* DMSK  */
    aud_gpio_set(s_candidates[(unsigned)s_idx], 8, 1);   /* DIR = output */
    aud_gpio_set(s_candidates[(unsigned)s_idx], 0x18, 0);/* IE off */
    aud_gpio_set(s_candidates[(unsigned)s_idx], 0, 1);   /* DATA = high */
    return (int)s_candidates[(unsigned)s_idx];
#else
    return 0;
#endif
}

/* ---- beep: real PCM via the VBC DA buffer (first integration) ----------- */

void audio_beep(uint16_t freq_hz, uint16_t ms)
{
#if !defined(HOST_TEST)
    uint32_t start, i, n, v;

    /* VBC DA data registers (vbc_phy_v5.h): VBDAL/VBDAR @ base+0/4 are the
     * 16-bit DAC sample ports; VB_CLK_CTL bits 5/6/8 (DA0/DA1/ANA on);
     * VBBUFFERSIZE @ base+0x10; VBENABLE bit 15 @ base+0x18. Written by
     * ARM directly (vbc_phy_v5.c __vbc_write: REG32(VBDAL)=l, REG32(VBDAR)=r).
     * The codec DAC then drives EAR_L/R -> NCP2817 amp -> speaker. */
    const uint32_t vb_base      = 0x82003000u;
    const uint32_t vb_ctl       = 0x8B0001C4u;  /* = APB_PERI_CTL0 (VB_CLK_CTL) */
    const uint32_t vb_anaon     = (1u << 8);
    const uint32_t vb_da0on     = (1u << 5);
    const uint32_t vb_da1on     = (1u << 6);
    const uint32_t vb_dabufdta  = vb_base + 0x18u;
    const uint32_t vb_buffsize  = vb_base + 0x10u;
    const uint32_t vb_bufsiz    = 160u;          /* VB_DA_BUF_SIZE (words) */
    const int16_t  amp          = 0x1800;        /* ~19% duty square */

    audio_power_on();
    aud_dac_path_on();
    audio_dac_on();
    audio_dp_dac_on();
    audio_pa_enable();
    audio_pa_mute(0);

    /* Enable the VBC DA path (ANA + DA0 + DA1 clocks). */
    v = MEM4(vb_ctl) | vb_anaon | vb_da0on | vb_da1on;
    MEM4(vb_ctl) = v;

    /* VBC-side routing the SDK's playback always sets (vbc_phy_v5.c):
     *   VBIISSEL @ base+0x3C: DA port = IIS_PORT_NORMAL (0) — selects the
     *   IIS port the VBC DA data is clocked out on to the codec DAC.
     *   DAPATHCTL @ base+0x40: DA ADDFM = NO_MIX (0) — no FM mixing. */
    v = MEM4(vb_base + 0x3Cu) & ~3u;
    MEM4(vb_base + 0x3Cu) = v;
    v = MEM4(vb_base + 0x40u) & ~3u;
    MEM4(vb_base + 0x40u) = v;

    /* Set the DA buffer size (VBBUFFERSIZE low 8 bits = size-1). */
    v = MEM4(vb_buffsize) & ~0xFFu;
    MEM4(vb_buffsize) = v | (vb_bufsiz - 1u);

    /* Square wave: alternating +/- amp, ~18 samples @ 8 kHz = 440 Hz
     * (VBC runs at 8 kHz). Fill both ping-pong buffers. */
    n = (uint32_t)freq_hz > 0 ? (8000u / 2u / (uint32_t)freq_hz) : 9u;
    if (n < 2u) n = 2u;
    for (i = 0; i < vb_bufsiz; i++) {
        int16_t s = ((i / n) & 1u) ? amp : (int16_t)(-amp);
        MEM4(vb_base + 0x00u) = (uint32_t)(uint16_t)s;
        MEM4(vb_base + 0x04u) = (uint32_t)(uint16_t)s;
    }

    /* Enable the VBC: VBENABLE bit 15 starts DA playback. */
    v = MEM4(vb_dabufdta) | (1u << 15);
    MEM4(vb_dabufdta) = v;

    /* DAC_DATA_TX_ADDR probe (audif_sync_ctl @ 0x82001288 bits 0-1): the
     * codec DAC's input-source select, never written by SDK AP code. Cycle
     * all four values, each with a DISTINCT tone, so ONE flash identifies
     * the working source by which frequency is heard:
     *   addr 0 = 440 Hz, addr 1 = 660 Hz, addr 2 = 880 Hz, addr 3 = 990 Hz */
    {
        const uint16_t probe_freq[4] = { 440, 660, 880, 990 };
        uint32_t p;

        for (p = 0; p < 4; p++) {
            uint32_t w;
            uint32_t half_n, j;

            w = aud_adi_read(0x82001288u);
            aud_adi_write(0x82001288u, (w & ~3u) | p);
            aud_delay_ms(3);

            half_n = 8000u / 2u / (uint32_t)probe_freq[p];
            if (half_n < 2u) half_n = 2u;
            for (j = 0; j < vb_bufsiz; j++) {
                int16_t s = ((j / half_n) & 1u) ? amp : (int16_t)(-amp);
                MEM4(vb_base + 0x00u) = (uint32_t)(uint16_t)s;
                MEM4(vb_base + 0x04u) = (uint32_t)(uint16_t)s;
            }

            start = sched_ticks();
            while (sched_ticks() - start < 300u)
                task_yield();
        }
    }

    /* Stop: VBENABLE off, mute, power down. */
    v = MEM4(vb_dabufdta) & ~(1u << 15);
    MEM4(vb_dabufdta) = v;
    v = MEM4(vb_ctl) & ~(vb_anaon | vb_da0on | vb_da1on);
    MEM4(vb_ctl) = v;

    audio_pa_mute(1);
    audio_power_off();
#else
    (void)freq_hz; (void)ms;
#endif
}

/* ---- kernel module ------------------------------------------------------- */

int audio_init(void)
{
    /* No codec power at module time: the beep test powers on/off per call
     * so a bad register guess cannot wedge the boot path. */
    return 0;
}

const module_t audio_module = { "audio", audio_init };
