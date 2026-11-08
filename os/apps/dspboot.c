/* B310E OS — Tools / DSP boot: a boot check for os/drivers/dsp.c.
 *
 * Boots the DSP from this phone's NOR in a short-lived thread, then checks
 * that the program really runs (dsp_probe(): its parameter records, idle
 * frames, the stock's ping and memory dump), shows what came back and holds
 * the DSP in reset again. Nothing uses the radio. The whole report and the
 * dump are on the card: /b310e-os/dsp-report.txt, dsp-share.bin, dsp-dump.bin.
 * Center runs it again.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/dsp.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"

screen_t *app_dspboot(void);

/* words, for os/tests/test-dsp.py (symbol dspboot_status); append only */
volatile struct {
    uint32_t runs;                      /* finished boot attempts */
    uint32_t busy;
    int32_t result;                     /* dsp_boot() */
} dspboot_status;

static const char *const phases[] = {
    "booting...", "started, watching", "sending records", "idle frames", "ping", "memory dump",
    "saving",
};

static void worker(void *arg)
{
    dsp_probe_info.phase = 0;
    /* Our own program first: does the DSP execute at all? */
    dsp_selftest();
    /* The stock program with start-up marks, then as the stock sends it (the
     * report and the screen show this last run; the marks go in the report). */
    dsp_set_trace(true);
    if (dsp_boot() == DSP_OK) {
        dsp_probe();
        dsp_selftest_info.trace_start = dsp_probe_info.trace_start;
        dsp_selftest_info.trace_records = dsp_probe_info.trace_records;
        dsp_selftest_info.trace_end = dsp_probe_info.trace_end;
    }
    dsp_stop();
    dsp_set_trace(false);
    dsp_probe_info.phase = 0;
    dspboot_status.result = dsp_boot();
    if (dspboot_status.result == DSP_OK)
        dsp_probe();
    dsp_stop();
    dspboot_status.runs++;
    dspboot_status.busy = 0;
}

static void start(void)
{
    if (dspboot_status.busy)
        return;
    dspboot_status.busy = 1;
    if (!thread_create("dspboot", PRIO_NORMAL, 2048, worker, NULL))
        dspboot_status.busy = 0;
}

static void row(int y, const char *label, const char *value, uint16_t c)
{
    ui_kv(UI_PAD + 6, y, LCD_WIDTH - 2 * UI_PAD - 12, label, value, c);
}

static void draw(screen_t *s)
{
    char buf[24];
    int y = UI_TOP + 4, w = LCD_WIDTH - 2 * UI_PAD;
    volatile dsp_probe_t *p = &dsp_probe_info;
    uint16_t c;

    gfx_round(UI_PAD, y, w, 100, UI_RADIUS, C_CARD);
    if (dspboot_status.busy) {
        snprintf(buf, sizeof(buf), "%s", p->phase < 7 ? phases[p->phase] : "...");
        c = C_TEXT;
    } else if (!dspboot_status.runs) {
        snprintf(buf, sizeof(buf), "-");
        c = C_TEXT;
    } else if (dspboot_status.result != DSP_OK) {
        snprintf(buf, sizeof(buf), "%s", dsp_strerror(dspboot_status.result));
        c = C_BAD;
    } else {
        const char *v = dsp_probe_verdict();

        snprintf(buf, sizeof(buf), "%s", v);
        c = v[0] == 'r' ? C_OK : C_WARN;
    }
    /* the result alone on its line: an error's text is long */
    gfx_text_centered(y + 6, buf, c, FONT_SMALL, 1);
    snprintf(buf, sizeof(buf), "%lu, %lu ms", (unsigned long)dsp_info.blocks,
             (unsigned long)dsp_info.boot_ms);
    row(y + 20, "Blocks", buf, C_TEXT);
    snprintf(buf, sizeof(buf), "%lu/%lu, %lu", (unsigned long)p->records_taken,
             (unsigned long)p->records_total, (unsigned long)dsp_info.param_blocks);
    row(y + 32, "Records", buf, C_TEXT);
    snprintf(buf, sizeof(buf), "%lu, %lu", (unsigned long)p->replies, (unsigned long)dsp_info.irqs);
    row(y + 44, "Replies, IRQs", buf, C_TEXT);
    snprintf(buf, sizeof(buf), "%04lx", (unsigned long)dsp_selftest_info.trace_end);
    row(y + 56, "Marks", buf, C_TEXT);
    snprintf(buf, sizeof(buf), "%lu, %lu us", (unsigned long)p->idle_consumed,
             (unsigned long)p->frame_us);
    row(y + 68, "Frames", buf, C_TEXT);
    snprintf(buf, sizeof(buf), "%lu %lu", (unsigned long)dsp_selftest_info.banks_set.busy,
             (unsigned long)dsp_selftest_info.banks_clear.busy);
    row(y + 80, "Own program", buf, C_TEXT);
    snprintf(buf, sizeof(buf), "%lu, %lu/%lu", (unsigned long)p->dump_blocks,
             (unsigned long)p->rec_match, (unsigned long)p->rec_words);
    row(y + 92, "Dump, match", buf, C_TEXT);
    gfx_text_centered(UI_BOTTOM - 12, "Run: again; report on card", C_DIM, FONT_SMALL, 1);
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

static screen_t dspboot_screen = {
    .title = "DSP boot", .center = "Run", .right = "Back",
    .enter = enter, .draw = draw, .key = key, .tick = tick, .tick_ms = 200,
};

screen_t *app_dspboot(void)
{
    return &dspboot_screen;
}
