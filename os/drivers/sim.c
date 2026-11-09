/* B310E OS — SIM card reader: power a slot, read its ATR, run T=0
 * commands. No modem or DSP involved; polled, thread context.
 *
 * Ported from the Rockbox port's sim-sc6530c.c (verified on the phone
 * 2026-11-06: slot 0 read a real card at 1.8 V, slot 1 empty reported no
 * card). Controller, clocks, supply and activation come from the stock
 * sim_drv.c (NOR 0x36400..0x37400), see docs/sim.md; the T=0
 * procedure-byte handling and the ATR parsing follow ISO/IEC 7816-3.
 *
 * Differences from the Rockbox reader: the ATR is parsed (protocols,
 * class indicator, TCK) and a 1.8 V answer whose class indicator leaves
 * out class C is retried at 3 V; an inverse-convention ATR is converted
 * in software before the controller's convention is toggled (Rockbox only
 * patched TS); case-2 P3 = 0 reads 256 bytes; every wait is bounded; the
 * CPU clock is held at 104 MHz or more while a slot is active (as SD and
 * audio do: whether the SIM clock follows the AHB is not known).
 * Pins: the boot pinmux table sets the SIM pads; nothing here writes
 * pinmux.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/sim.h"

#define SIM_BASE 0x85000000u
#define SIM_TX   REG32(SIM_BASE + 0x00)
#define SIM_RX   REG32(SIM_BASE + 0x04)
#define SIM_STS0 REG32(SIM_BASE + 0x08)
#define SIM_STS1 REG32(SIM_BASE + 0x0c)
#define SIM_IE   REG32(SIM_BASE + 0x10)
#define SIM_ICLR REG32(SIM_BASE + 0x14)
#define SIM_CTL0 REG32(SIM_BASE + 0x18)
#define SIM_CTL1 REG32(SIM_BASE + 0x1c)
#define SIM_RXCD REG32(SIM_BASE + 0x20)
#define SIM_SHE  REG32(SIM_BASE + 0x24)
#define SIM_TGC  REG32(SIM_BASE + 0x28)
#define SIM_WDT  REG32(SIM_BASE + 0x2c)
#define SIM_IMSK REG32(SIM_BASE + 0x30)
#define SIM_TXCD REG32(SIM_BASE + 0x34)
#define SIM_WDT1 REG32(SIM_BASE + 0x38)
#define SIM_CTL2 REG32(SIM_BASE + 0x3c)
#define SIM_STS2 REG32(SIM_BASE + 0x40)

#define CTL0_CONV      0x0003u     /* convention / logic level */
#define CTL0_FIFO_RST  0x0030u     /* RX (b4) and TX (b5) FIFO reset */
#define CTL0_TXFIFO    0x0020u
#define CTL0_RST       0x0040u     /* card RST pin */
#define CTL0_POWER     0x0080u
#define CTL0_RX_EN     0x0200u
#define CTL0_TX_EN     0x0400u
#define CTL0_IO_LOW    0x8000u
#define CTL1_CLK_EN    0x0800u
#define RX_COUNT()     (SIM_STS2 & 0x7f)
#define TX_COUNT()     ((SIM_STS2 >> 8) & 0x7f)
#define ETU_CLOCKS     0xba0u      /* 8 x 372: 26 MHz / 8 = 3.25 MHz, F=372 D=1 */
#define TX_FIFO_FULL   32

/* APB (Rockbox / stock clock devices SIM0, SIM1) */
#define APB_EN_SET     0x8b0000a0u
#define APB_EN_CLR     0x8b0000a4u
#define APB_SIM_GATES  0x02000004u /* bit 25 (SIM0) and bit 2 (SIM1): both */
#define APB_RST_SET    0x8b000060u
#define APB_RST_CLR    0x8b000064u
#define APB_SIM_RST    0x00100000u /* bit 20 */
#define APB_SIM_SEL    0x8b0001e0u /* bits 3:2 slot; bit 1 is the FM I2C pads */
#define APB_SIM_STOP   0x8b000220u /* clock-stop level, bit 1 << slot */

/* ANA (through the ADI bridge) */
#define LDO_PD_SET     0x82001184u /* VSIM power-down: set = off */
#define LDO_PD_CLR     0x820011a4u /* set = on */
#define LDO_VSIM_V     0x820011c8u /* VSIM0 bits 7:4, VSIM1 bits 11:8 */
#define VSIM_1V8       0xau        /* stock 0x355f6: level 0 */
#define VSIM_3V        0x6u        /* level 2 */

/* times, ms (Rockbox: HZ / 50 + 1 ticks at HZ 100, and so on) */
#define SETTLE_MS      30          /* LDO settle, clock before RST (stock >= 1 / 20) */
#define RETRY_MS       100         /* deactivated before the next class */
#define ATR_FIRST_MS   300
#define ATR_GAP_MS     50
#define WWT_MS         1100        /* WWT ~1.1 s at WI = 10 */
#define BYTE_MS        1000
#define TX_MS          500

static mutex_t lock;                /* zero is an unlocked mutex */
static int active = -1;
static bool held;                   /* cpufreq_hold() taken */

static uint32_t elapsed(uint32_t start)
{
    return kernel_ticks() - start;
}

/* ---- supply ---------------------------------------------------------------- */

static void ana_update(uint32_t addr, uint32_t clear, uint32_t set)
{
    uint32_t s = arch_irq_save();   /* other drivers share these registers */

    adi_write(addr, (adi_read(addr) & ~clear) | set);
    arch_irq_restore(s);
}

static void vsim(int slot, bool on, bool three_volt)
{
    uint32_t bit = slot ? 0x4u : 0x2u;
    uint32_t shift = slot ? 8 : 4;

    if (on) {
        ana_update(LDO_VSIM_V, 0xfu << shift, (three_volt ? VSIM_3V : VSIM_1V8) << shift);
        ana_update(LDO_PD_SET, bit, 0);
        ana_update(LDO_PD_CLR, 0, bit);
    } else {
        ana_update(LDO_PD_CLR, bit, 0);
        ana_update(LDO_PD_SET, 0, bit);
        ana_update(LDO_VSIM_V, 0xfu << shift, VSIM_1V8 << shift);
    }
}

static void apb_update(uint32_t addr, uint32_t clear, uint32_t set)
{
    uint32_t s = arch_irq_save();

    REG32(addr) = (REG32(addr) & ~clear) | set;
    arch_irq_restore(s);
}

/* ---- byte level ------------------------------------------------------------ */

/* One byte, or -1 after ms. */
static int sim_getc(uint32_t ms)
{
    uint32_t start = kernel_ticks();

    while (!RX_COUNT()) {
        if (elapsed(start) >= ms)
            return -1;
        thread_sleep(1);            /* ~1.1 ms a character; the FIFO holds more */
    }
    return (int)(SIM_RX & 0xff);
}

static bool sim_tx_drain(void)
{
    uint32_t start = kernel_ticks();

    while (TX_COUNT())
        if (elapsed(start) >= TX_MS)
            return false;
    udelay(3000);                   /* last character + guard time */
    return true;
}

static bool sim_send(const uint8_t *data, int len)
{
    bool ok = true;

    SIM_CTL0 &= ~CTL0_TX_EN;
    SIM_CTL0 |= CTL0_TXFIFO;
    udelay(1);
    SIM_CTL0 &= ~CTL0_TXFIFO;
    SIM_CTL0 |= CTL0_TX_EN;
    for (int i = 0; i < len && ok; i++) {
        uint32_t start = kernel_ticks();

        while (TX_COUNT() >= TX_FIFO_FULL)
            if (elapsed(start) >= TX_MS) {
                ok = false;
                break;
            }
        if (ok)
            SIM_TX = data[i];
    }
    ok = sim_tx_drain() && ok;
    SIM_CTL0 &= ~CTL0_TX_EN;        /* turn the line around */
    return ok;
}

/* ---- ATR -------------------------------------------------------------------- */

static uint8_t inverse_byte(uint8_t b)
{
    uint8_t r = 0;

    for (int i = 0; i < 8; i++)
        if (b & (1u << i))
            r |= (uint8_t)(0x80u >> i);
    return (uint8_t)~r;
}

void sim_atr_parse(sim_atr_t *a)
{
    unsigned i = 2, level = 1, y, k, need;
    bool tck = false, t15_next = false, have_classes = false, td1 = false;

    a->complete = a->tck_ok = false;
    a->proto = 0;
    a->protocols = 0;
    a->classes = 0;
    a->ta1 = a->tc1 = -1;
    a->hist_off = a->hist_len = 0;
    if (a->len < 2)
        return;
    y = a->raw[1] >> 4;
    k = a->raw[1] & 0xf;
    for (;;) {
        if (y & 1) {                /* TAi */
            if (i >= a->len)
                return;
            if (level == 1)
                a->ta1 = a->raw[i];
            if (t15_next && !have_classes) {
                a->classes = a->raw[i] & 0x3f;
                have_classes = true;
            }
            i++;
        }
        if (y & 2)                  /* TBi */
            i++;
        if (y & 4) {                /* TCi */
            if (i >= a->len)
                return;
            if (level == 1)
                a->tc1 = a->raw[i];
            i++;
        }
        if (!(y & 8))
            break;
        if (i >= a->len)            /* TDi */
            return;
        {
            unsigned td = a->raw[i++], t = td & 0xf;

            if (level == 1) {
                a->proto = (uint8_t)t;
                td1 = true;
            }
            if (t)
                tck = true;
            t15_next = t == 15;
            if (t != 15)
                a->protocols |= (uint16_t)(1u << t);
            y = td >> 4;
            level++;
        }
        if (level > 16)
            return;
    }
    if (!td1 || !a->protocols)
        a->protocols |= 1;          /* no TD1: T=0 only */
    if (a->proto == 15)
        a->proto = 0;
    a->hist_off = (uint8_t)(i < a->len ? i : a->len);
    a->hist_len = (uint8_t)k;
    need = i + k + (tck ? 1 : 0);
    a->complete = need <= a->len;
    if (!tck) {
        a->tck_ok = true;
    } else if (a->complete) {
        uint8_t x = 0;

        for (unsigned j = 1; j < need; j++)
            x ^= a->raw[j];
        a->tck_ok = x == 0;
    }
}

/* ---- activation ------------------------------------------------------------- */

/* Stock 0x36faa -> 0x36f26. */
static void power_down(int slot)
{
    SIM_IE = 0;
    SIM_CTL0 &= ~CTL0_RST;
    udelay(50);
    SIM_CTL1 &= ~CTL1_CLK_EN;
    udelay(50);
    SIM_CTL0 |= CTL0_IO_LOW;
    udelay(50);
    vsim(slot, false, false);
    REG32(APB_EN_CLR) = APB_SIM_GATES;      /* SIM0/SIM1 APB clocks off */
    active = -1;
    if (held) {
        held = false;
        cpufreq_release();
    }
}

/* Cold reset at one class; true with a valid TS. */
static bool power_up(int slot, bool three_volt, sim_atr_t *atr)
{
    uint32_t n;
    int c;

    if (!held) {
        held = true;
        cpufreq_hold();
    }
    active = slot;
    /* Clocks (stock clock devices SIM0/SIM1, ops 0x673c0..): APB enable
     * and reset bit 20. Slot select and clock-stop level per slot. */
    REG32(APB_EN_SET) = APB_SIM_GATES;
    REG32(APB_RST_SET) = APB_SIM_RST;
    udelay(10);
    REG32(APB_RST_CLR) = APB_SIM_RST;
    apb_update(APB_SIM_SEL, 0xcu, (uint32_t)slot << 2);
    apb_update(APB_SIM_STOP, 2u << slot, 0);

    vsim(slot, true, three_volt);
    thread_sleep(SETTLE_MS);

    /* Stock 0x36ea0 InitAfterLDOValid + 0x36b46 clock setup. */
    SIM_CTL2 = 1;
    SIM_CTL1 = (SIM_CTL1 & ~0x1f00u) | 0x200;   /* 26 MHz / 8 */
    SIM_RXCD = ETU_CLOCKS;
    SIM_TXCD = ETU_CLOCKS;
    SIM_STS0 = 0;
    SIM_STS1 = 0;
    SIM_IE = 0;
    SIM_ICLR = 0x1ff;
    SIM_CTL0 = 0x2003;
    SIM_SHE = 0;
    SIM_TGC = 0x505;
    SIM_WDT = 0x802;
    SIM_WDT1 = 0x100;
    SIM_IMSK = 0;

    /* Stock 0x36d90 ClearBeforeReset. */
    for (n = 0; RX_COUNT() && n < 256; n++)
        (void)SIM_RX;
    SIM_ICLR = 0xffff;
    SIM_SHE = 0x0a;
    SIM_CTL2 = (SIM_CTL2 & 0xff00) | 1;

    /* Cold reset: clock on, RST low, stock 0x36dd0 StartToReset, then RST
     * high after >= 400 clocks (stock HAL waits 20 ms). */
    SIM_CTL1 |= CTL1_CLK_EN;
    thread_sleep(SETTLE_MS);
    SIM_CTL0 &= ~CTL0_RST;
    SIM_CTL0 |= CTL0_RX_EN | CTL0_TX_EN;
    SIM_CTL0 |= CTL0_POWER;
    SIM_CTL1 |= CTL1_CLK_EN;
    udelay(30);
    SIM_CTL0 &= ~CTL0_IO_LOW;
    thread_sleep(SETTLE_MS);
    SIM_CTL0 |= CTL0_RST;

    /* ATR: the first byte within 300 ms, the end after 50 ms of silence. */
    memset(atr, 0, sizeof(*atr));
    atr->ta1 = atr->tc1 = -1;
    atr->cls = three_volt ? SIM_CLASS_B : SIM_CLASS_C;
    atr->mv = three_volt ? 3000 : 1800;
    c = sim_getc(ATR_FIRST_MS);
    while (c >= 0 && atr->len < SIM_ATR_MAX) {
        atr->raw[atr->len++] = (uint8_t)c;
        c = sim_getc(ATR_GAP_MS);
    }
    if (atr->len && atr->raw[0] == 0x03) {
        /* Inverse convention, read as direct: each byte is reversed and
         * complemented. Convert, then switch the controller (stock 0x36b18
         * toggles CTL0 bits 0-1). Not seen on the phone yet. */
        for (unsigned i = 0; i < atr->len; i++)
            atr->raw[i] = inverse_byte(atr->raw[i]);
        atr->inverse = true;
        SIM_CTL0 ^= CTL0_CONV;
    }
    if (!atr->len || (atr->raw[0] != 0x3b && atr->raw[0] != 0x3f))
        return false;
    sim_atr_parse(atr);
    return true;
}

static int activate_locked(int slot, int cls, sim_atr_t *atr)
{
    bool first = true;
    int result = SIM_ENOCARD;

    if (active >= 0)
        power_down(active);         /* again on the same slot: a cold reset */
    for (int c = SIM_CLASS_C; c <= SIM_CLASS_B; c++) {
        if (cls && c != cls)
            continue;
        if (!first) {
            thread_sleep(RETRY_MS);
        }
        first = false;
        if (!power_up(slot, c == SIM_CLASS_B, atr)) {
            result = atr->len ? SIM_EBADATR : SIM_ENOCARD;
            power_down(slot);
            continue;
        }
        /* A class indicator without class C: try 3 V; if that gives
         * nothing, take the 1.8 V answer again. */
        if (!cls && c == SIM_CLASS_C && atr->classes && !(atr->classes & 4)) {
            power_down(slot);
            thread_sleep(RETRY_MS);
            if (power_up(slot, true, atr))
                return SIM_OK;
            power_down(slot);
            thread_sleep(RETRY_MS);
            if (power_up(slot, false, atr))
                return SIM_OK;
            power_down(slot);
            return atr->len ? SIM_EBADATR : SIM_ENOCARD;
        }
        return SIM_OK;
    }
    return result;
}

int sim_activate_class(int slot, int cls, sim_atr_t *atr)
{
    int r;

    if (slot < 0 || slot >= SIM_SLOTS || !atr ||
        (cls != 0 && cls != SIM_CLASS_C && cls != SIM_CLASS_B))
        return SIM_EINVAL;
    mutex_lock(&lock, WAIT_FOREVER);
    if (active >= 0 && active != slot)
        r = SIM_EBUSY;
    else
        r = activate_locked(slot, cls, atr);
    mutex_unlock(&lock);
    return r;
}

int sim_activate(int slot, sim_atr_t *atr)
{
    return sim_activate_class(slot, 0, atr);
}

void sim_deactivate(int slot)
{
    mutex_lock(&lock, WAIT_FOREVER);
    if (slot >= 0 && slot == active)
        power_down(slot);
    mutex_unlock(&lock);
}

int sim_active_slot(void)
{
    return active;
}

/* ---- T=0 -------------------------------------------------------------------- */

static int t0(const uint8_t hdr[5], const uint8_t *data, uint8_t *resp, int *got)
{
    int len = hdr[4], done = 0;

    if (resp && !len)
        len = 256;
    if (!data && !resp)
        len = 0;
    /* Stock 0x36e46: FIFO reset, RX mark 1, RX+TX on, retransmit 3/3. */
    SIM_CTL0 |= CTL0_FIFO_RST;
    udelay(1);
    SIM_CTL0 &= ~CTL0_FIFO_RST;
    SIM_CTL2 = (SIM_CTL2 & 0xff00) | 1;
    SIM_CTL0 |= CTL0_RX_EN | CTL0_TX_EN;
    SIM_SHE = 0x33;
    SIM_WDT = 0x803;
    if (!sim_send(hdr, 5))
        return SIM_ETIMEOUT;
    for (int nulls = 0; nulls < 100;) {
        int pb = sim_getc(WWT_MS);

        if (pb < 0)
            return SIM_ETIMEOUT;
        if (pb == 0x60) {           /* NULL: more time */
            nulls++;
            continue;
        }
        if ((pb & 0xf0) == 0x60 || (pb & 0xf0) == 0x90) {
            int sw2 = sim_getc(BYTE_MS);

            *got = done;
            return sw2 < 0 ? SIM_ETIMEOUT : pb << 8 | sw2;
        }
        bool all = pb == hdr[1];
        bool one = pb == (hdr[1] ^ 0xff);
        if (!all && !one)
            return SIM_EPROTO;      /* not a procedure byte */
        int n = all ? len - done : 1;
        if (n > len - done)
            n = len - done;
        if (data) {
            if (n > 0 && !sim_send(data + done, n))
                return SIM_ETIMEOUT;
            done += n;
        } else if (resp) {
            for (int i = 0; i < n; i++) {
                int c = sim_getc(BYTE_MS);

                if (c < 0)
                    return SIM_ETIMEOUT;
                resp[done++] = (uint8_t)c;
            }
        }
    }
    return SIM_ETIMEOUT;
}

int sim_apdu(int slot, const uint8_t hdr[5], const uint8_t *data,
             uint8_t *resp, int *resp_len)
{
    int r, got = 0;

    if (!hdr || (data && resp))
        return SIM_EINVAL;
    mutex_lock(&lock, WAIT_FOREVER);
    r = slot >= 0 && slot == active ? t0(hdr, data, resp, &got) : SIM_ENOTACTIVE;
    mutex_unlock(&lock);
    if (resp_len)
        *resp_len = got;
    return r;
}

/* GSM authentication (TS 51.011 RUN GSM ALGORITHM): SELECT MF, SELECT DF_GSM,
 * run A3/A8 on the 16-octet RAND, then GET RESPONSE for SRES (4) + Kc (8). The
 * slot must already be active. Returns SIM_OK, or the SW / a negative error. The
 * driver logs nothing of the RAND, SRES or Kc. */
int sim_run_gsm_algo(int slot, const uint8_t rand[16], uint8_t sres[4], uint8_t kc[8])
{
    static const uint8_t mf[2]  = { 0x3f, 0x00 };
    static const uint8_t gsm[2] = { 0x7f, 0x20 };
    uint8_t hdr[5] = { 0xa0, 0xa4, 0x00, 0x00, 0x02 };  /* SELECT, Lc 2 */
    uint8_t resp[16];
    int sw, got = 0;

    sw = sim_apdu(slot, hdr, mf, 0, 0);                 /* SELECT MF */
    if (sw < 0)
        return sw;
    sw = sim_apdu(slot, hdr, gsm, 0, 0);                /* SELECT DF_GSM */
    if (sw < 0)
        return sw;
    hdr[1] = 0x88;                                      /* RUN GSM ALGORITHM */
    hdr[4] = 0x10;                                      /* Lc 16 (RAND) */
    sw = sim_apdu(slot, hdr, rand, 0, 0);
    if (sw < 0)
        return sw;
    if ((sw & 0xff00) != 0x9f00)                        /* expect 9F 0C */
        return sw;
    hdr[1] = 0xc0;                                      /* GET RESPONSE */
    hdr[4] = (uint8_t)(sw & 0xff);
    sw = sim_apdu(slot, hdr, 0, resp, &got);
    if (sw < 0)
        return sw;
    if (got < 12)
        return sw > 0 ? sw : SIM_EPROTO;
    for (int i = 0; i < 4; i++)
        sres[i] = resp[i];
    for (int i = 0; i < 8; i++)
        kc[i] = resp[4 + i];
    return SIM_OK;
}

const char *sim_strerror(int err)
{
    switch (err) {
    case SIM_OK:          return "ok";
    case SIM_ENOCARD:     return "no card";
    case SIM_EBADATR:     return "bad ATR";
    case SIM_ETIMEOUT:    return "timeout";
    case SIM_EPROTO:      return "protocol error";
    case SIM_EBUSY:       return "busy";
    case SIM_EINVAL:      return "invalid";
    case SIM_ENOTACTIVE:  return "not active";
    default:              return err > 0 ? "status" : "error";
    }
}
