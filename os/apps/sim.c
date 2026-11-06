/* B310E OS — the SIM screen: for each slot, whether a card answers, the
 * supply it answered at, the protocol from its ATR, the ATR length, and
 * the status words of a GSM SELECT of the MF (3F00). Center reads again.
 *
 * The slots are read by a short-lived thread, so the UI keeps running
 * (an empty slot takes about half a second: no ATR at 1.8 V, then 3 V).
 * Nothing read from the card is shown, logged or kept beyond the ATR's
 * protocol bytes and status words: no ICCID, no IMSI, no historical bytes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/sim.h"
#include "os/ui.h"

screen_t *app_sim(void);

#define LINE_H 10

enum { SLOT_IDLE, SLOT_READING, SLOT_DONE };
#define F_INVERSE    1u
#define F_TCK_BAD    2u
#define F_SHORT      4u

/* One slot's result; all words, so the QEMU test (os/tests/test-sim.py)
 * reads it through the ELF symbol sim_app_status. Append only. */
typedef struct {
    uint32_t state;                 /* SLOT_* */
    int32_t result;                 /* sim_activate(): SIM_OK or an error */
    uint32_t mv;                    /* supply the card answered at */
    uint32_t proto;                 /* first offered protocol */
    uint32_t protocols;             /* bit n: T=n */
    uint32_t atr_len;
    int32_t sw;                     /* SELECT MF: SW1SW2, an error, 0 not run */
    uint32_t flags;                 /* F_* */
} sim_slot_report_t;

volatile struct {
    uint32_t scans;                 /* finished reads of both slots */
    uint32_t busy;
    sim_slot_report_t slot[SIM_SLOTS];
} sim_app_status;

static void probe(int slot)
{
    static const uint8_t select_mf[5] = { 0xa0, 0xa4, 0x00, 0x00, 0x02 };
    static const uint8_t mf[2] = { 0x3f, 0x00 };
    volatile sim_slot_report_t *r = &sim_app_status.slot[slot];
    sim_atr_t atr;
    int rc, sw = 0;

    r->state = SLOT_READING;
    rc = sim_activate(slot, &atr);
    if (rc == SIM_OK) {
        sw = sim_apdu(slot, select_mf, mf, NULL, NULL);
        sim_deactivate(slot);
    }
    r->result = rc;
    r->mv = rc == SIM_OK ? atr.mv : 0;
    r->proto = rc == SIM_OK ? atr.proto : 0;
    r->protocols = rc == SIM_OK ? atr.protocols : 0;
    r->atr_len = atr.len;
    r->sw = sw;
    r->flags = rc != SIM_OK ? 0 : (atr.inverse ? F_INVERSE : 0) |
               (atr.tck_ok ? 0 : F_TCK_BAD) | (atr.complete ? 0 : F_SHORT);
    r->state = SLOT_DONE;

    /* protocol bytes and status words only */
    if (rc == SIM_OK)
        klog("sim: slot %d card at %u mV, T=%u (protocols 0x%x, classes 0x%x, "
             "ta1 %02x, %s%s%s), ATR %u bytes, SELECT MF %s%04x", slot,
             atr.mv, atr.proto, atr.protocols, atr.classes, atr.ta1 & 0xff,
             atr.inverse ? "inverse" : "direct", atr.tck_ok ? "" : ", TCK bad",
             atr.complete ? "" : ", short", atr.len,
             sw < 0 ? sim_strerror(sw) : "", sw < 0 ? 0 : sw);
    else
        klog("sim: slot %d %s (%u ATR bytes)", slot, sim_strerror(rc), atr.len);
}

static void reader(void *arg)
{
    for (int s = 0; s < SIM_SLOTS; s++)
        probe(s);
    sim_app_status.scans++;
    sim_app_status.busy = 0;
}

static void start(void)
{
    if (sim_app_status.busy)
        return;
    sim_app_status.busy = 1;
    for (int s = 0; s < SIM_SLOTS; s++)
        sim_app_status.slot[s].state = SLOT_IDLE;
    if (!thread_create("sim", PRIO_NORMAL, 2048, reader, NULL))
        sim_app_status.busy = 0;
}

static void protocols_text(char *buf, size_t n, uint32_t proto, uint32_t mask)
{
    /* the first offered, then any other of T=0 / T=1 */
    uint32_t other = mask & 3u & ~(1u << proto);

    if (other)
        snprintf(buf, n, "T=%lu,%lu", (unsigned long)proto,
                 (unsigned long)(other & 1u ? 0 : 1));
    else
        snprintf(buf, n, "T=%lu", (unsigned long)proto);
}

static void draw(screen_t *s)
{
    char buf[32], pt[12];
    int y = UI_TOP + 4;

    for (int i = 0; i < SIM_SLOTS; i++) {
        volatile sim_slot_report_t *r = &sim_app_status.slot[i];
        uint16_t c = C_TEXT;

        if (r->state == SLOT_READING || (r->state == SLOT_IDLE && sim_app_status.busy)) {
            snprintf(buf, sizeof(buf), "SIM %d: reading...", i + 1);
            c = C_DIM;
        } else if (r->state == SLOT_IDLE) {
            snprintf(buf, sizeof(buf), "SIM %d: -", i + 1);
            c = C_DIM;
        } else if (r->result == SIM_OK) {
            snprintf(buf, sizeof(buf), "SIM %d: card", i + 1);
            c = C_OK;
        } else if (r->result == SIM_ENOCARD) {
            snprintf(buf, sizeof(buf), "SIM %d: no card", i + 1);
            c = C_DIM;
        } else {
            snprintf(buf, sizeof(buf), "SIM %d: %s", i + 1, sim_strerror(r->result));
            c = C_BAD;
        }
        gfx_text(2, y, buf, c, FONT_SMALL, 1, LCD_WIDTH - 4);
        y += LINE_H + 2;
        if (r->state == SLOT_DONE && r->result == SIM_OK) {
            protocols_text(pt, sizeof(pt), r->proto, r->protocols);
            snprintf(buf, sizeof(buf), " %s V %s ATR %lu%s%s",
                     r->mv >= 3000 ? "3" : "1.8", pt, (unsigned long)r->atr_len,
                     r->flags & F_INVERSE ? " inv" : "",
                     r->flags & (F_TCK_BAD | F_SHORT) ? " !" : "");
            gfx_text(2, y, buf, C_TEXT, FONT_SMALL, 1, LCD_WIDTH - 4);
            y += LINE_H;
            if (r->sw > 0) {
                bool ok = r->sw == 0x9000 || (r->sw >> 8) == 0x9f;

                snprintf(buf, sizeof(buf), " MF 3F00: %04lX %s",
                         (unsigned long)r->sw, ok ? "ok" : "");
                gfx_text(2, y, buf, ok ? C_OK : C_WARN, FONT_SMALL, 1, LCD_WIDTH - 4);
            } else {
                snprintf(buf, sizeof(buf), " MF 3F00: %s", sim_strerror(r->sw));
                gfx_text(2, y, buf, C_BAD, FONT_SMALL, 1, LCD_WIDTH - 4);
            }
            y += LINE_H;
        } else if (r->state == SLOT_DONE && r->atr_len) {
            snprintf(buf, sizeof(buf), " ATR %lu bytes", (unsigned long)r->atr_len);
            gfx_text(2, y, buf, C_DIM, FONT_SMALL, 1, LCD_WIDTH - 4);
            y += LINE_H;
        }
        y += 8;
    }
}

static void enter(screen_t *s)
{
    start();
}

static void tick(screen_t *s)
{
}

static void key(screen_t *s, const key_event_t *ev)
{
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT)
        start();
    else if (ev->code == KEY_RSOFT)
        ui_pop();
}

static screen_t sim_screen = {
    .title = "SIM", .center = "Read", .right = "Back",
    .enter = enter, .draw = draw, .key = key, .tick = tick, .tick_ms = 200,
};

screen_t *app_sim(void)
{
    return &sim_screen;
}
