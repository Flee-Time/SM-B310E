/* B310E OS — Tools / DSP boot: a boot check for os/drivers/dsp.c.
 *
 * Boots the DSP from this phone's NOR in a short-lived thread, shows how
 * far the download got (stage, blocks, the DSP_READY word, the time),
 * then holds the DSP in reset again. Nothing else is sent to it.
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

static const char *const stages[] = {
    "-", "image found", "decoded", "reset", "ROM ready", "downloaded", "started",
};

static void worker(void *arg)
{
    dspboot_status.result = dsp_boot();
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
    gfx_text(UI_PAD + 6, y, label, C_DIM, FONT_SMALL, 1, 0);
    gfx_text(LCD_WIDTH - UI_PAD - 6 - gfx_text_width(value, FONT_SMALL, 1), y, value, c,
             FONT_SMALL, 1, 0);
}

static void draw(screen_t *s)
{
    char buf[24];
    int y = UI_TOP + 4, w = LCD_WIDTH - 2 * UI_PAD;
    uint32_t stage = dsp_info.stage;
    bool ok = dspboot_status.runs && !dspboot_status.busy && dspboot_status.result == DSP_OK;

    gfx_round(UI_PAD, y, w, 86, UI_RADIUS, C_CARD);
    if (dspboot_status.busy)
        snprintf(buf, sizeof(buf), "booting...");
    else if (!dspboot_status.runs)
        snprintf(buf, sizeof(buf), "-");
    else
        snprintf(buf, sizeof(buf), "%s", ok ? "booted" : dsp_strerror(dspboot_status.result));
    row(y + 6, "Result", buf, ok ? C_OK : dspboot_status.runs ? C_BAD : C_TEXT);
    row(y + 20, "Stage", stages[stage < 7 ? stage : 0], C_TEXT);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)dsp_info.blocks);
    row(y + 34, "Blocks", buf, C_TEXT);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)dsp_info.program_words);
    row(y + 48, "Halfwords", buf, C_TEXT);
    snprintf(buf, sizeof(buf), "%04lx", (unsigned long)dsp_info.ready_word);
    row(y + 62, "Ready word", buf, C_TEXT);
    snprintf(buf, sizeof(buf), "%lu ms", (unsigned long)dsp_info.boot_ms);
    row(y + 76, "Time", buf, C_TEXT);
    gfx_text_centered(UI_BOTTOM - 12, "the DSP is reset after", C_DIM, FONT_SMALL, 1);
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
