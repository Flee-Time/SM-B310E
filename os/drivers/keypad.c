/* B310E OS — keypad matrix and the END key.
 *
 * The matrix controller (0x87000000, IRQ 8) is edge-triggered: int_raw
 * bits 0-3 are press edges of columns 0-3, bits 4-7 release edges; the
 * row/column of each is in key_status, one byte per edge slot. Ported
 * from the Rockbox port's button-sc6530c.c (verified on the phone) and
 * fpdoom (Unlicense). END is not in the matrix: it is a level on analog
 * EIC channel 3, polled every 20 ms by a small thread, which also polls
 * the headset (EIC channel 4, the in-line button comparator, and the jack;
 * drivers/audio.c classifies the buttons) and posts KEY_HS_* events.
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
    key_event_t ev = { code, down, kernel_ticks() };

    if (events.buf)
        queue_send_isr(&events, &ev);
}

static void keypad_isr(void)
{
    uint32_t raw = KPD->int_raw, status = KPD->key_status;

    if (raw)
        KPD->int_clr = KPD_INT_ALL;
    if (status & 8u)                    /* a bad scan frame */
        return;
    for (int i = 0; i < 8; i++) {
        uint32_t byte, k;

        if (!(raw & (1u << i)))
            continue;
        byte = status >> ((i & 3) * 8);
        k = (byte & 0x70u) >> 1 | (byte & 7u);
        if (keymap[k] != 0xff)
            post(keymap[k], i < 4);
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

static void end_key_main(void *arg)
{
    static const struct { unsigned bit; uint8_t code; } hs[] = {
        { AUDIO_HS_CENTER, KEY_HS_CENTER }, { AUDIO_HS_PLUS, KEY_HS_PLUS },
        { AUDIO_HS_MINUS, KEY_HS_MINUS },
    };
    bool was = false;
    unsigned hs_was = 0;
    uint32_t end_since = 0;

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
            klog("END held %u s: the UI did not respond, power off", END_FAILSAFE_MS / 1000);
            arch_power_off();
        }
        for (unsigned i = 0; i < ARRAY_SIZE(hs); i++)
            if ((held ^ hs_was) & hs[i].bit)
                post(hs[i].code, (held & hs[i].bit) != 0);
        hs_was = held;
        thread_sleep(20);
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

    irq_register(IRQ_KEYPAD, keypad_isr);
    irq_enable_line(IRQ_KEYPAD);
    thread_create("endkey", PRIO_HIGH, 1024, end_key_main, NULL);
}

int keypad_get(key_event_t *ev, uint32_t timeout_ms)
{
    return queue_recv(&events, ev, timeout_ms);
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
