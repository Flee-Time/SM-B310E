/* B310E OS — keypad matrix and the END key.
 *
 * The matrix controller (0x87000000, IRQ 8) is edge-triggered: int_raw
 * bits 0-3 are press edges of columns 0-3, bits 4-7 release edges; the
 * row/column of each is in key_status, one byte per edge slot. Ported
 * from the Rockbox port's button-sc6530c.c (verified on the phone) and
 * fpdoom (Unlicense). END is not in the matrix: it is a level on analog
 * EIC channel 3, read by a small thread, which also reads the headset (EIC
 * channel 4, the in-line button comparator, and the jack; drivers/audio.c
 * classifies the buttons) and posts KEY_HS_* events. The thread waits for
 * the EIC's interrupt when it works and nothing is held, else polls every
 * 20 ms (see "END and the headset button by interrupt" below).
 *
 * Events go to a queue; keypad_get() takes them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/audio.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"

#define KPD_BASE    0x87000000u
#define KPD_INT_ALL 0xfffu
#define EIC_DATA    0x82001900u
#define EIC_DMSK    0x82001904u
#define EIC_END     3
#define EIC_HOOK    4                   /* headset button, stock EIC 20 */
#define END_FAILSAFE_MS 8000
#define UI_STUCK_MS     3000

typedef volatile struct {
    uint32_t ctrl, int_en, int_raw, int_mask;
    uint32_t int_clr, dummy_14, polarity, debounce;
    uint32_t long_key, sleep_cnt, clk_divide, key_status;
} kpd_t;

#define KPD ((kpd_t *)KPD_BASE)

/* stock keymap (dump 0xc6e70): index = row * 8 + column */
static const uint8_t keymap[64] = {
    0x0d, 0x01, 0xff, 0xff, 0x06, 0xff, 0xff, 0xff,
    0x31, 0x32, 0x33, 0x08, 0x05, 0xff, 0xff, 0xff,
    0x34, 0x35, 0x36, 0x09, 0xff, 0xff, 0xff, 0xff,
    0x37, 0x38, 0x39, 0xff, 0x04, 0xff, 0xff, 0xff,
    0x2a, 0x30, 0x23, 0xff, 0x07, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static queue_t events;

static void post(uint8_t code, bool down)
{
    key_event_t ev = { code, down, kernel_ticks(), 0 };

    if (events.buf)
        queue_send_isr(&events, &ev);
}

/* The matrix keys down now, by code: a press of one already down is
 * not posted again. Re-enabling the scan (keypad_resume, after every
 * card DMA) latches the keys held at that moment as new presses: Doom,
 * reading the card all the time, got dozens a press, and the flood
 * filled the event queue until a release was lost. */
static uint32_t matrix_down[4];
volatile uint32_t ui_alive_at;
volatile bool ui_end_held;

static void keypad_isr(void)
{
    uint32_t raw = KPD->int_raw, status = KPD->key_status;

    if (raw)                            /* what was read: a later edge stays. All of
                                         * them lost a release that came just now */
        KPD->int_clr = raw;
    if (status & 8u)                    /* a bad scan frame */
        return;
    for (int i = 0; i < 8; i++) {
        uint32_t byte, k;

        if (!(raw & (1u << i)))
            continue;
        byte = status >> ((i & 3) * 8);
        k = (byte & 0x70u) >> 1 | (byte & 7u);
        if (keymap[k] != 0xff) {
            uint8_t code = keymap[k];
            uint32_t bit = 1u << (code & 31), *word = &matrix_down[(code >> 5) & 3];

            if (i < 4) {
                if (*word & bit)
                    continue;           /* still held: no second press */
                *word |= bit;
            } else {
                *word &= ~bit;
            }
            post(code, i < 4);
        }
    }
}

void keypad_pause(void)
{
    KPD->ctrl &= ~1u;
}

void keypad_resume(void)
{
    uint32_t s = arch_irq_save();

    keypad_isr();                       /* collect, acknowledge */
    KPD->ctrl |= 1u;
    arch_irq_restore(s);
}

/* ---- END and the headset button by interrupt ---------------------------
 *
 * The stock firmware's way (eic_phy_v5.c, NOR 0x39074..0x39dd0; the GPIO
 * init at 0x24db8 and EICA_DbncIsr at 0x39602): the debounce EIC is level
 * triggered, so each channel's trigger level (IEV) is set to the opposite
 * of its level now; the analog die's interrupt block (ANA_INT, 0x82001580,
 * enable +8, the EIC is bit 5) raises INTC line 24. The handler here only
 * masks the line and wakes the END thread, which reads the levels, posts
 * the keys and arms again (clear IC, enable IE, retrigger TRIG). While a
 * key is held, or a headset is in (its + and - are ADC levels), the thread
 * polls every 20 ms as before; otherwise it sleeps until an edge, waking
 * once a second for the jack. At start the thread checks the interrupt
 * comes (the trigger set to the level now fires at once); when it does
 * not, as in QEMU, which does not model it, it keeps polling. */
#define EIC_IEV      0x82001914u        /* 1: trigger on high */
#define EIC_IE       0x82001918u
#define EIC_MIS      0x82001920u
#define EIC_IC       0x82001924u        /* write 1 to clear */
#define EIC_TRIG     0x82001928u        /* restart the debounce */
#define EIC_CTRL(n)  (0x82001940u + 4u * (n))
#define EIC_KEYS     (1u << EIC_END | 1u << EIC_HOOK)
#define ANA_INT_EN   0x82001588u
#define ANA_INT_EIC  (1u << 5)
#define IRQ_ANA      24
#define IDLE_WAIT_MS 1000               /* the jack, and a safety net */

static sem_t eic_sem;
static bool eic_irq;                    /* the interrupt works: wait for it */
volatile uint32_t eic_irq_count;        /* diagnostics */

static void ana_isr(void)
{
    irq_disable_line(IRQ_ANA);          /* the thread acks the EIC (ADI) */
    eic_irq_count++;
    sem_post(&eic_sem);
}

/* trigger when a key's level differs from level (the bits of EIC_DATA) */
static void eic_arm(uint32_t iev)
{
    adi_write(EIC_IEV, (adi_read(EIC_IEV) & ~EIC_KEYS) | (iev & EIC_KEYS));
    adi_write(EIC_IC, adi_read(EIC_IC) | EIC_KEYS);
    adi_write(EIC_IE, adi_read(EIC_IE) | EIC_KEYS);
    adi_write(EIC_TRIG, adi_read(EIC_TRIG) | EIC_KEYS);
    irq_enable_line(IRQ_ANA);
}

static void eic_disarm(void)
{
    irq_disable_line(IRQ_ANA);
    adi_write(EIC_IE, adi_read(EIC_IE) & ~EIC_KEYS);
    adi_write(EIC_IC, adi_read(EIC_IC) | EIC_KEYS);
}

static void eic_irq_setup(void)
{
    sem_init(&eic_sem, 0);
    for (unsigned ch = EIC_END; ch <= EIC_HOOK; ch++)   /* debounce 50, on */
        adi_write(EIC_CTRL(ch), (adi_read(EIC_CTRL(ch)) & ~0xfffu) | 50u | 1u << 14);
    adi_write(EIC_IE, adi_read(EIC_IE) & ~EIC_KEYS);
    adi_write(ANA_INT_EN, ANA_INT_EIC); /* only the EIC (stock clears it at boot) */
    irq_register(IRQ_ANA, ana_isr);
}

/* the trigger set to the levels now: a working interrupt comes at once */
static bool eic_irq_check(void)
{
    uint32_t level = adi_read(EIC_DATA);
    bool ok;

    eic_arm(level);
    ok = sem_wait(&eic_sem, 100) == WAIT_OK;
    eic_disarm();
    while (sem_wait(&eic_sem, 0) == WAIT_OK)
        ;
    klog("end key: %s (EIC mis %04lx)", ok ? "by interrupt" : "polled, no EIC interrupt",
         (unsigned long)adi_read(EIC_MIS));
    return ok;
}

static void end_key_main(void *arg)
{
    static const struct { unsigned bit; uint8_t code; } hs[] = {
        { AUDIO_HS_CENTER, KEY_HS_CENTER }, { AUDIO_HS_PLUS, KEY_HS_PLUS },
        { AUDIO_HS_MINUS, KEY_HS_MINUS },
    };
    bool was = false;
    unsigned hs_was = 0;
    uint32_t end_since = 0;

    eic_irq = eic_irq_check();
    for (;;) {
        uint32_t eic = adi_read(EIC_DATA), t = kernel_ticks();
        bool now = (eic >> EIC_END) & 1;
        unsigned held = audio_headset_tick((eic >> EIC_HOOK) & 1, t);

        if (now != was) {
            was = now;
            end_since = t;
            post(KEY_END, now);
        }
        /* The UI powers off after 2 s of END. If it has not after 8 s, it
         * is stuck (an FM seek once blocked it for minutes): power off from
         * here, without touching the card. */
        if (now && t - end_since >= END_FAILSAFE_MS) {
            if (t - ui_alive_at > UI_STUCK_MS) {
                klog("END held %u s: the UI did not respond, power off", END_FAILSAFE_MS / 1000);
                arch_power_off();
            }
            /* It runs, but this press never got to its own power off (it woke the
             * screen, or closed an app): cleanly, so a recording's file is kept. */
            ui_end_held = true;
        }
        for (unsigned i = 0; i < ARRAY_SIZE(hs); i++)
            if ((held ^ hs_was) & hs[i].bit)
                post(hs[i].code, (held & hs[i].bit) != 0);
        hs_was = held;
        if (eic_irq && !now && !(eic & 1u << EIC_HOOK) && !audio_headphones()) {
            /* nothing held, no headset: until an edge, or a second */
            static unsigned idle_wakes;

            eic_arm(~eic);
            if (sem_wait(&eic_sem, IDLE_WAIT_MS) == WAIT_OK &&
                !((adi_read(EIC_DATA) ^ eic) & EIC_KEYS)) {
                /* woken with nothing changed: a bounce, once; many in a
                 * row would starve the UI (this thread is high priority) */
                if (++idle_wakes >= 8) {
                    eic_irq = false;
                    klog("end key: EIC interrupts without a change, polled from now");
                }
            } else {
                idle_wakes = 0;
            }
            eic_disarm();
            continue;
        }
        /* 20 ms while the screen is on; with the panel asleep 100 ms is
         * plenty for END to wake it and saves four wake-ups in five */
        thread_sleep(lcd_asleep() ? 100 : 20);
    }
}

void keypad_init(void)
{
    unsigned row = 0, col = 0;
    uint32_t ctrl;

    queue_init(&events, sizeof(key_event_t), 32);
    for (unsigned c = 0; c < 8; c++)
        for (unsigned r = 0; r < 8; r++)
            if (keymap[r * 8 + c] != 0xff)
                col |= 1u << c, row |= 1u << r;
    row &= 0xfcu;
    col &= 0xfcu;

    REG32(0x8b0000a0) |= 0x80040u;       /* keypad clock */
    KPD->int_clr = KPD_INT_ALL;
    KPD->clk_divide = 1;
    KPD->debounce = 16;
    KPD->int_en = KPD_INT_ALL;
    KPD->polarity = 0xffffu;
    ctrl = KPD->ctrl;
    ctrl |= 1u | 4u;                     /* enable, long-press detect */
    ctrl &= ~2u;
    ctrl &= ~(0xfcu << 16 | 0xfcu << 8);
    ctrl |= row << 16 | col << 8;
    KPD->ctrl = ctrl;

    adi_write(0x820010e4, 0x20);         /* EIC clocks, END channel */
    adi_write(0x820010e0, 0x80);
    adi_write(EIC_DMSK, adi_read(EIC_DMSK) | 1u << EIC_END | 1u << EIC_HOOK);
    eic_irq_setup();

    irq_register(IRQ_KEYPAD, keypad_isr);
    irq_enable_line(IRQ_KEYPAD);
    thread_create("endkey", PRIO_HIGH, 1024, end_key_main, NULL);
}

int keypad_get(key_event_t *ev, uint32_t timeout_ms)
{
    return queue_recv(&events, ev, timeout_ms);
}

/* A key from elsewhere (the network shell), or code 0: only wakes the UI
 * loop (ui_call). */
void keypad_inject(uint8_t code, bool down)
{
    post(code, down);
}

const char *keypad_name(uint8_t code)
{
    static const struct { uint8_t code; const char *name; } names[] = {
        { KEY_DIAL, "Dial" }, { KEY_END, "End" }, { KEY_UP, "Up" },
        { KEY_DOWN, "Down" }, { KEY_LEFT, "Left" }, { KEY_RIGHT, "Right" },
        { KEY_LSOFT, "LSoft" }, { KEY_RSOFT, "RSoft" },
        { KEY_CENTER, "Center" }, { KEY_HASH, "#" }, { KEY_STAR, "*" },
        { KEY_0, "0" }, { KEY_1, "1" }, { KEY_2, "2" }, { KEY_3, "3" },
        { KEY_4, "4" }, { KEY_5, "5" }, { KEY_6, "6" }, { KEY_7, "7" },
        { KEY_8, "8" }, { KEY_9, "9" }, { KEY_HS_CENTER, "Headset" },
        { KEY_HS_PLUS, "Headset +" }, { KEY_HS_MINUS, "Headset -" },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(names); i++)
        if (names[i].code == code)
            return names[i].name;
    return "?";
}
