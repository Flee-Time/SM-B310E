/* B310E boot loader — the early decision, chip init, keypad, ADI, timer.
 *
 * Register facts come from the B310E OS and Rockbox drivers (verified on
 * the phone) and from what fpdoom's sdboot and menu did on it. Every wait
 * is bounded. Nothing here uses interrupts.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "boot.h"

#define APB_EB_SET   0x8b0000a0u        /* clock enables, write 1 to set */
#define APB_EB_CLR   0x8b0000a4u
#define APB_RST_SET  0x8b000060u
#define APB_RST_CLR  0x8b000064u
#define CLK_CTL      0x8b000040u
#define SYS_MS       0x8100300cu        /* the 1 ms system counter */
#define KPD_BASE     0x87000000u

typedef volatile struct {
    uint32_t ctrl, int_en, int_raw, int_mask;
    uint32_t int_clr, dummy_14, polarity, debounce;
    uint32_t long_key, sleep_cnt, clk_divide, key_status;
} kpd_t;

#define KPD ((kpd_t *)KPD_BASE)

EARLY void delay(uint32_t n)
{
    while (n--)
        __asm__ volatile ("" ::: "memory");
}

EARLY uint32_t ms_now(void)
{
    uint32_t a, b = REG32(SYS_MS);

    do {
        a = b;
        b = REG32(SYS_MS);
    } while (a != b);
    return a;
}

EARLY void ms_wait(uint32_t ms)
{
    uint32_t start = ms_now(), n = ms * 200000u + 1000u;

    while (ms_now() - start < ms && --n)
        ;
}

/* ---- the early decision: stack only, no .data or .bss ------------------ */

/* The ADI up, before the first analog read (sdboot's init), and the system
 * counter, which ms_now() reads (off after a reset). */
EARLY static void early_adi(void)
{
    REG32(APB_EB_SET) = 0x80000u;       /* the system counter */
    REG32(APB_EB_SET) = 1u << 24;       /* ADI clock */
    REG32(APB_RST_SET) = 1u << 19;
    delay(100);
    REG32(APB_RST_CLR) = 1u << 19;
    REG32(0x82000000) &= ~(1u << 4);
    REG32(0x82000004) = 0x55000;
}

/* Take our request (boot.h, REQ_REG); leave anything else. *raw: the
 * stock's reset-mode byte, non-zero after a reset. */
EARLY static uint32_t early_request(uint32_t *raw)
{
    uint32_t v = adi_read(REQ_REG), b = (v >> 8) & 0xff;

    *raw = (REG32(HWRST_REG) >> 8) & 0xff;
    if (b < REQ_STOCK || b > REQ_PATH)
        return 0;
    /* For the stock firmware the whole register, as at a cold power-on: its
     * restart mode left in the low byte with 0x82001144 clear (a reset does
     * that) reads as a watchdog crash (NOR 0x1a89c, 0x1063c). */
    adi_write(REQ_REG, b == REQ_STOCK ? 0 : v & 0xff);
    return b;
}

/* Was the phone switched on with END? The power key is analog EIC
 * channel 3 (B310E OS keypad.c): the ADI up (sdboot's init), the EIC
 * clocks, the channel unmasked, its debounce on (B310E OS sets 50 for it;
 * a short one here) and restarted, then its debounced level for up to
 * 300 ms. v1.2 waited 40 ms with the debounce as the reset left it and
 * never saw END on the phone. A charger plugged into a phone that is off
 * powers it up without END. */
#define EIC_DATA   0x82001900u
#define EIC_DMSK   0x82001904u
#define EIC_TRIG   0x82001928u
#define EIC_CTRL2  0x82001948u          /* channel 2: the charger */
#define EIC_CTRL3  0x8200194cu          /* channel 3: debounce, bit 14 on */

/* END (channel 3) seen held in the window, and the charger (channel 2,
 * B310E OS battery.c) at its end: *charger. */
#define END_HOLD_MS 250
#define END_UP_MS   20
EARLY static bool early_end(bool *charger)
{
    uint32_t t, since = 0, last = 0, n = 1000000;   /* n: never hang a boot */
    bool down;

    adi_write(0x820010e4, 0x20);        /* EIC clocks, the END channel */
    adi_write(0x820010e0, 0x80);
    /* Only unmask END and the charger, and read. v1.3 to v1.10 also set
     * the debounce of both channels and restarted it (EIC_CTRL2/3,
     * EIC_TRIG); after a jump the stock firmware then booted with its
     * display off until END was pressed, as if not switched on by the
     * power key. The mask is kept for stock_jump() to put back. */
    REG32(EARLY_SAVE_DMSK) = adi_read(EIC_DMSK);
    adi_write(EIC_DMSK, adi_read(EIC_DMSK) | 1u << 3 | 1u << 2);
    /* END seen once was enough until v1.19: taps in a row woke the loader,
     * and the next tap fell inside the wait this one had opened. It now has
     * to stay down END_HOLD_MS; END_UP_MS up is a tap (not a bounce). */
    t = ms_now();
    down = false;
    do {
        uint32_t d = adi_read(EIC_DATA), now = ms_now();

        *charger = d & 1u << 2;
        if (d & 1u << 3) {
            if (!down)
                since = now;
            down = true;
            last = now;
            if (now - since >= END_HOLD_MS)
                return true;
        } else if (down && now - last >= END_UP_MS) {
            return false;
        }
    } while ((down || ms_now() - t < 300) && --n);
    return false;
}

/* The keypad light n times (start.S: the NOR past 4 MiB read wrong,
 * so the stock firmware follows instead of the menu). */
EARLY void early_signal(int n)
{
    while (n--) {
        adi_write(0x82001224, (adi_read(0x82001224) & ~0xf0u) | 0xe0u);
        adi_write(0x82001224, (adi_read(0x82001224) & ~0x30u) | 0x20u);
        ms_wait(200);
        adi_write(0x82001224, adi_read(0x82001224) & ~0x30u);
        adi_write(0x82001224, adi_read(0x82001224) | 0x10u);
        ms_wait(200);
    }
}

/* Off, from the boot decision (.early: B310E OS's power-off order, the
 * supplies only: nothing is lit yet). */
EARLY static void early_power_off(void)
{
    uint32_t n = 1000000;

    if (!adi_write(0x82001184, 0x1f))   /* LDO_PD_SET1 */
        return;
    while ((REG32(0x82000020) & (1u << 9)) && --n)
        ;
    REG32(0x82001180) = 0x3fff;         /* LDO_PD_SET0: the CPU's own */
    ms_wait(2000);
}

/* 0: chain the stock firmware now. Else (request << 8) | EARLY_STAY,
 * with EARLY_END (END held).
 *
 * The phone runs this whenever it gets power: a battery put in, a charger
 * plugged in, END pressed, a reset. The stock firmware's "off" is its own
 * code waiting for END (v1.6 on the phone: the menu came up at every
 * battery insertion; END pressed in the stock's wait started the stock
 * without the loader). So:
 *   END held, or a request of ours                 the loader stays
 *   the stock's restart mode 8 (END on its charging
 *   screen, END possibly let go by now)            the menu
 *   a charger                                      the stock (charging)
 *   a stock reset reason in the byte (0x1f ...)    the stock
 *   request 0xb0                                   the stock, at once
 *   anything else (a battery put in)               switched off: the next
 *                                                  END is a real power-on
 * Build default "stock" chains the stock whenever the loader would not
 * stay, as sdboot did. */
EARLY uint32_t boot_early(void)
{
    uint32_t raw, req, entry = _start.stock_entry;
    bool stock_ok = entry && entry != 0xffffffffu && !(entry & 3);
    bool end, charger = false, asked;

    early_adi();
    req = early_request(&raw);
    if (req == REQ_STOCK && stock_ok)
        return 0;
    end = early_end(&charger);
    asked = (adi_read(REQ_REG) & STOCK_MODE_MASK) == STOCK_MODE_START;
    if (!req && stock_ok) {
        if (_start.mode == MODE_STOCK)
            return 0;
        if (asked) {
            end = true;                 /* as a power-on with END held */
        } else if (!end) {
            if (charger || raw)
                return 0;
            early_power_off();
            return 0;                   /* no power-off: the stock waits */
        }
    }
    if (asked)                          /* once: not again at the next reset */
        adi_write(REQ_REG, adi_read(REQ_REG) & ~STOCK_MODE_MASK);
    return req << 8 | EARLY_STAY | (end ? EARLY_END : 0);
}

/* ---- ADI: the analog die through the mailbox --------------------------- */

#define ADI_RD_CMD   0x82000018u
#define ADI_RD_DATA  0x8200001cu
#define ADI_STS      0x82000020u
#define ADI_FULL     (1u << 9)
#define ADI_EMPTY    (1u << 8)
#define ADI_BUDGET   1000000u

EARLY uint32_t adi_read(uint32_t addr)
{
    uint32_t data, n = ADI_BUDGET;

    while (!(REG32(ADI_STS) & ADI_EMPTY) && --n)
        ;
    REG32(ADI_RD_CMD) = addr & 0xfff;
    n = ADI_BUDGET;
    do {
        data = REG32(ADI_RD_DATA);
    } while ((data >> 31) && --n);
    return data & 0xffff;
}

EARLY bool adi_write(uint32_t addr, uint32_t value)
{
    uint32_t n = ADI_BUDGET;

    while ((REG32(ADI_STS) & ADI_FULL) && --n)
        ;
    if (!n)
        return false;
    REG32(addr) = value;
    n = ADI_BUDGET;
    while (!(REG32(ADI_STS) & ADI_EMPTY) && --n)
        ;
    return n != 0;
}

/* ---- chip init: what sdboot did with a key held ------------------------ */

/* 208 MHz, the AHB at half (104 MHz): the divider first, then the source
 * (stock 0x33ca4; B310E OS clock.c expects this at entry). */
static void clock_init(void)
{
    uint32_t a = REG32(CLK_CTL);

    REG32(CLK_CTL) = a |= 4;
    delay(100);
    REG32(CLK_CTL) = (a & ~3u) | 1;
    delay(100);
}

/* The PSRAM controller (SMC 0x20000000). The two 16-bit stores into
 * PSRAM program the chip's configuration register (address-coded); they
 * change 0x04000020 and 0x0410323e. Runs from NOR: not from PSRAM. */
static void smc_init(void)
{
    const uint32_t base = 0x20000000u, ps = 0x04000000u;

    REG32(base + 0x00) = 0x22220000;
    REG32(base + 0x04) = 0;
    REG32(base + 0x20) = 0;
    REG32(base + 0x24) = 0x00924ff0;
    REG32(base + 0x28) = 0x0151ffff;
    REG32(base + 0x2c) = 0x00a0744f;
    delay(100);
    REG32(base + 0x00) = 0x222211e0;
    REG32(base + 0x04) = 0x8080;
    delay(10);
    REG32(base + 0x24) |= 0x20000;
    REG16(ps + 0x10323e) = 0;
    REG16(ps + 0x20) = 0;
    delay(10);
    REG32(base + 0x24) &= ~0x20000u;
    delay(10);
    REG32(base + 0x24) = 0x00ac1fff;
    REG32(base + 0x28) = 0x015115ff;
    REG32(base + 0x2c) = 0x00501015;
    REG32(base + 0x00) = 0x222210e0;
    REG32(base + 0x04) = 0x8080;
    delay(100);
}

static void adi_init(void)
{
    REG32(APB_EB_SET) = 1u << 24;       /* ADI clock */
    REG32(APB_RST_SET) = 1u << 19;
    delay(100);
    REG32(APB_RST_CLR) = 1u << 19;
    REG32(0x82000000) &= ~(1u << 4);
    REG32(0x82000004) = 0x55000;
}

static void watchdog_stop(void)
{
    adi_write(0x820014a0, 0xe551);      /* WDG 0x82001480: unlock */
    adi_write(0x82001488, adi_read(0x82001488) & ~0xfu);
    adi_write(0x820014a0, (uint16_t)~0xe551u);
}

/* IRAM banks 1..3 (APB 0x8b0001a0 bits 19-21): off at power-on, so IRAM
 * from about 0x4000c000 did not hold data (v1.4's "FAT error 12" and
 * square corners). fpdoom's menu set them at entry (entry.c), so every
 * payload it started had them; B310E OS and Rockbox bounce SD DMA through
 * IRAM 0x4000b000..0x4000f000 (reads over 8 blocks came back wrong: a
 * Rockbox MP3 crashed, found by the OS drivers session 2026-11-08). Bits
 * 15:0 and 16 are the DSP boot address and ARM_BOOT_EN: left alone. */
#define IRAM_BANKS_REG  0x8b0001a0u
#define IRAM_BANKS      (7u << 19)

void chip_init(void)
{
    REG32(0x8000000c) = 0xffffffffu;    /* INTC: every line masked */
    REG32(IRAM_BANKS_REG) |= IRAM_BANKS;
    clock_init();
    smc_init();
    adi_init();
    watchdog_stop();
    REG32(APB_EB_SET) = 0x80000u;       /* the system counter */
    REG32(APB_EB_CLR) = 0xc00;          /* UART clocks off (the fpdoom menu did) */
}

/* ---- back to the stock firmware without a reset ------------------------
 *
 * "Stock firmware" in the menu enters the stock PBL at 0x46e4 as the
 * no-key path does, after a full init. The PBL runs its full init (its
 * warm-boot word at 0x04000000 is cleared first, so it does not take the
 * fast path that assumes its own init already ran). It starts from the
 * clock and PSRAM controller values the chip had at power-on, as saved by
 * chip_save() before chip_init(), with our peripherals stopped. (In
 * August a jump from a RAM-loaded menu hung, but that menu ran with the
 * PSRAM window moved to 0x34000000; this loader never moves it.) */
static const uint8_t smc_offsets[] = { 0x24, 0x28, 0x2c, 0x20, 0x04, 0x00 };
static uint32_t saved_clk, saved_smc[sizeof(smc_offsets)];
static bool saved;

uint32_t poweron_backlight, poweron_keylight, poweron_lcd_rst;
static uint32_t poweron_iram;

void chip_save(void)
{
    /* the display side, for lcd_off() before the stock (the ADI is up:
     * boot_early) */
    poweron_backlight = adi_read(0x82001220);
    poweron_keylight = adi_read(0x82001224);
    poweron_lcd_rst = REG32(0x8b000224u);
    poweron_iram = REG32(IRAM_BANKS_REG) & IRAM_BANKS;
    saved_clk = REG32(CLK_CTL);
    for (unsigned i = 0; i < sizeof(smc_offsets); i++)
        saved_smc[i] = REG32(0x20000000u + smc_offsets[i]);
    saved = true;
}

void stock_jump(void)
{
    uint32_t v;

    REG32(0x8000000c) = 0xffffffffu;    /* INTC: every line masked */
    KPD->ctrl &= ~1u;
    KPD->int_clr = 0xfff;
    REG32(APB_EB_CLR) = 0x40;           /* keypad clock off */
    REG32(LOAD_ADDR) = 0;               /* not the PBL's warm-boot magic */
    adi_write(0x82001904, REG32(EARLY_SAVE_DMSK));  /* the EIC mask, as found */
    /* The stock's reset-mode marker, cleared as at a cold power-on. While
     * it runs the stock firmware keeps 0x1f1f in analog 0x82001140 and
     * 0x82001148, the magic 0x7ee7 in 0x82001144, 0x1f in the APB byte
     * (NOR 0x1a5ac); the analog die keeps them while the battery is in. A
     * boot that finds them is a restart after a crash: no logo, the display
     * left off until a key (what the phone showed after the jump, v1.4 to
     * v1.12). */
    adi_write(0x82001140, 0);
    adi_write(0x82001144, 0);
    adi_write(0x82001148, 0);
    REG32(HWRST_REG) &= ~0xff00u;
    /* IRAM banks 1..3 as at power-on (the stack and .bss are below them) */
    REG32(IRAM_BANKS_REG) = (REG32(IRAM_BANKS_REG) & ~IRAM_BANKS) | poweron_iram;
    if (saved) {
        /* PSRAM is not used from here: code in NOR, stack in IRAM */
        for (unsigned i = 0; i < sizeof(smc_offsets); i++)
            REG32(0x20000000u + smc_offsets[i]) = saved_smc[i];
        delay(100);
        v = REG32(CLK_CTL);             /* going down: the source first */
        REG32(CLK_CTL) = (v & ~3u) | (saved_clk & 3u);
        delay(100);
        REG32(CLK_CTL) = saved_clk;
        delay(100);
    }
    boot_chain_stock(_start.stock_entry);
}

/* The stock firmware's pin table, read from this phone's NOR at run time
 * (as the fpdoom menu did at every boot): {address, value} pairs of
 * pinmux (0x8c000xxx) and analog (0x82001xxx) registers up to {-1, -1}.
 * The table is checked whole before anything is written; 0x8c0002a4 (the
 * UART TX pin, which hangs this phone) is never written. Returns the
 * pairs written, 0 when the table is not there. */
#define PINMAP_NOR   0x000c6ef0u
#define PINMAP_MAX   256
#define PIN_FORBIDDEN 0x8c0002a4u

int pinmap_apply(void)
{
    const volatile uint32_t *p = (const volatile uint32_t *)PINMAP_NOR;
    int n, i, done = 0;

    for (n = 0; n < PINMAP_MAX; n++) {
        uint32_t a = p[2 * n];

        if (a == 0xffffffffu && p[2 * n + 1] == 0xffffffffu)
            break;
        if (a >> 12 != 0x8c000 && a >> 12 != 0x82001)
            return 0;
        if (a & 3)
            return 0;
    }
    if (n < 10 || n == PINMAP_MAX || p[0] >> 12 != 0x8c000)
        return 0;
    for (i = 0; i < n; i++) {
        uint32_t a = p[2 * i], v = p[2 * i + 1];

        if (a == PIN_FORBIDDEN)
            continue;
        if (a >> 12 == 0x82001)
            adi_write(a, v & 0xffff);
        else
            REG32(a) = v;
        done++;
    }
    return done;
}

/* 0x82001220: bits 0-4 current, bit 6 on, 0x1100 kept (B310E OS). */
/* The backlight off as the stock firmware switches it off (NOR 0x1a186
 * with 0: bits 5-6 cleared, then bit 5; 0x1a218 with 0: 0x2200 set, then
 * 0x1100 cleared). Its LCD init reads bit 2 of 0x82001320 (set at an END
 * power-on, cleared once backlight_on() has run) and only initialises the
 * panel when it is set: with the light left on it took the display for
 * one already running. */
void backlight_off_stock(void)
{
    adi_write(0x82001220, adi_read(0x82001220) & ~0x60u);
    adi_write(0x82001220, adi_read(0x82001220) | 0x20u);
    adi_write(0x82001220, adi_read(0x82001220) | 0x2200u);
    adi_write(0x82001220, adi_read(0x82001220) & 0xeeffu);
}

void backlight_on(int percent)
{
    uint32_t v = (uint32_t)((percent * 32 + 50) / 100), t;

    t = (adi_read(0x82001220) & ~0x7fu) | 0x1100u;
    if (v)
        t |= v - 1u;
    adi_write(0x82001220, t);
    adi_write(0x82001220, t | (v ? 0x40u : 0x20u));
}

/* Switched off as B310E OS does (arch_power_off; stock NOR 0x1a448): the
 * lights off, then the supplies, SET1 = 0x1f, then SET0 = 0x3fff. With a
 * charger in, the phone comes back in the stock off-charging mode. */
void power_off(void)
{
    uint32_t n = 1000000;

    adi_write(0x82001224, adi_read(0x82001224) & ~0x30u);     /* keypad light */
    adi_write(0x82001224, adi_read(0x82001224) | 0x10u);
    backlight_on(0);
    REG32(0x8000000c) = 0xffffffffu;
    /* END is down after "held 2 s": cut now and the chip comes straight back
     * up with END held, which is a power-on to the menu. Display off, wait. */
    for (uint32_t t = ms_now(); (adi_read(0x82001900) & 1u << 3) && ms_now() - t < 30000; )
        ;
    ms_wait(50);
    if (!adi_write(0x82001184, 0x1f))
        return;
    while ((REG32(0x82000020) & (1u << 9)) && --n)
        ;
    REG32(0x82001180) = 0x3fff;
    ms_wait(2000);
}

/* The watchdog in 0.5 s (B310E OS arch_reboot, fpdoom sys_wdg_reset).
 * Returns after 2 s if no reset came (a QEMU run without a watchdog). */
void watchdog_reset(void)
{
    adi_write(0x820010e0, 4);
    adi_write(0x820010e4, 2);
    adi_write(0x820014a0, 0xe551);
    adi_write(0x82001488, adi_read(0x82001488) | 9);
    adi_write(0x82001480, 0x4000);
    adi_write(0x82001484, 0);
    adi_write(0x82001488, adi_read(0x82001488) | 2);
    adi_write(0x820014a0, (uint16_t)~0xe551u);
    ms_wait(2000);
}

/* ---- keypad for the menu ------------------------------------------------ */

/* index row * 8 + column (B310E OS keypad.c), rows 0-4 */
static const uint8_t keymap[40] = {
    0x0d, 0x01, 0xff, 0xff, 0x06, 0xff, 0xff, 0xff,
    0x31, 0x32, 0x33, 0x08, 0x05, 0xff, 0xff, 0xff,
    0x34, 0x35, 0x36, 0x09, 0xff, 0xff, 0xff, 0xff,
    0x37, 0x38, 0x39, 0xff, 0x04, 0xff, 0xff, 0xff,
    0x2a, 0x30, 0x23, 0xff, 0x07, 0xff, 0xff, 0xff,
};

void keypad_open(void)
{
    uint32_t ctrl;

    REG32(APB_EB_SET) = 0x80040u;
    KPD->int_clr = 0xfff;
    KPD->clk_divide = 1;
    KPD->debounce = 16;
    KPD->int_en = 0;
    KPD->polarity = 0xffff;
    ctrl = KPD->ctrl;
    ctrl |= 1u;
    ctrl &= ~(2u | 4u);
    ctrl &= ~(0xfcu << 16 | 0xfcu << 8);
    ctrl |= 0x1cu << 16 | 0x1cu << 8;   /* rows 2-4, columns 2-4 (+0, 1) */
    KPD->ctrl = ctrl;
}

int keypad_poll(void)
{
    uint32_t raw = KPD->int_raw, status = KPD->key_status;
    int code = 0;

    if (!raw)
        return 0;
    KPD->int_clr = 0xfff;
    if (status & 8u)                    /* a bad scan frame */
        return 0;
    for (int i = 0; i < 4; i++) {       /* press edges only */
        uint32_t byte, k;

        if (!(raw & (1u << i)))
            continue;
        byte = status >> (i * 8);
        k = (byte & 0x70u) >> 1 | (byte & 7u);
        if (k < sizeof(keymap) && keymap[k] != 0xff)
            code = keymap[k];
    }
    return code;
}

/* Until the keys are let go (a release edge) or ms pass: a program must
 * not get the release of the key that started it. */
void keypad_wait_release(uint32_t ms)
{
    uint32_t t = ms_now();

    while (ms_now() - t < ms) {
        uint32_t raw = KPD->int_raw;

        KPD->int_clr = raw;
        if (raw & 0xf0u)
            break;
    }
    ms_wait(30);
    KPD->int_clr = 0xfff;
}
