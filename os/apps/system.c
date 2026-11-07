/* B310E OS — system information: uptime, CPU load, memory, card, battery and
 * every thread with its share of the CPU over the last second.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/status.h"
#include "os/ui.h"

#define MAX_THREADS 24
#define LINE_H      11

static struct {
    thread_t *t;
    uint32_t last;
    uint16_t permille;
} seen[MAX_THREADS];
static uint32_t last_total, last_irqs, irqs_per_s;
static uint32_t last_line[32], line_per_s[32];
static int first;                       /* scroll position, in lines */

static void sample(void)
{
    uint32_t total = kernel_cycles(), span = total - last_total;
    uint32_t s = arch_irq_save();
    int n = 0;

    for (thread_t *t = thread_first(); t && n < MAX_THREADS; t = thread_next(t), n++) {
        uint32_t c = t->cycles;
        if (seen[n].t != t)
            seen[n].last = c;
        seen[n].permille = span >= 1000 ? (uint16_t)((c - seen[n].last) / (span / 1000)) : 0;
        seen[n].t = t;
        seen[n].last = c;
    }
    for (; n < MAX_THREADS; n++)
        seen[n].t = NULL;
    arch_irq_restore(s);
    irqs_per_s = span ? (uint32_t)((uint64_t)(irq_count - last_irqs) * 26000000u / span) : 0;
    last_irqs = irq_count;
    for (int i = 0; i < 32; i++) {
        uint32_t c = irq_line_count[i];

        line_per_s[i] = span ? (uint32_t)((uint64_t)(c - last_line[i]) * 26000000u / span) : 0;
        last_line[i] = c;
    }
    last_total = total;
}

/* One row: a dim label at the left, its value at the right. */
static int row(int y, int idx, const char *label, const char *value, uint16_t c)
{
    int r = idx - first, yy = y + r * LINE_H;

    if (r >= 0 && yy <= UI_BOTTOM - LINE_H) {
        int vw = gfx_text_width(value, FONT_SMALL, 1);

        gfx_text(UI_PAD + 2, yy, label, C_DIM, FONT_SMALL, 1, 0);
        gfx_text(LCD_WIDTH - UI_PAD - 2 - vw, yy, value, c, FONT_SMALL, 1, 0);
    }
    return idx + 1;
}

static int heading(int y, int idx, const char *text)
{
    int r = idx - first, yy = y + r * LINE_H;

    if (r >= 0 && yy <= UI_BOTTOM - LINE_H) {
        gfx_fill(UI_PAD, yy + 3, LCD_WIDTH - 2 * UI_PAD, 1, C_CARD_HI);
        gfx_fill(UI_PAD + 2, yy, gfx_text_width(text, FONT_SMALL, 1) + 6, 8, C_BG);
        gfx_text(UI_PAD + 5, yy, text, C_ACCENT, FONT_SMALL, 1, 0);
    }
    return idx + 1;
}

static void draw(screen_t *s)
{
    char v[32];
    const fs_info_t *fi = fs_info();
    sd_stats_t st;
    uint32_t t = kernel_ticks(), idle = kernel_idle_permille();
    uint32_t load = idle < 1000 ? 1000 - idle : 0;
    int mv = (int)os_status.battery_mv, y = UI_TOP + 4, i = 0;

    snprintf(v, sizeof(v), "%luh %02lum %02lus", (unsigned long)(t / 3600000),
             (unsigned long)(t / 60000 % 60), (unsigned long)(t / 1000 % 60));
    i = row(y, i, "Uptime", v, C_TEXT);
    snprintf(v, sizeof(v), "%d MHz, %lu.%lu%%", clock_mhz(),
             (unsigned long)(load / 10), (unsigned long)(load % 10));
    i = row(y, i, "CPU", v, load > 800 ? C_WARN : C_TEXT);
    snprintf(v, sizeof(v), "%lu/s%s", (unsigned long)irqs_per_s,
             irq_stray_count ? ", stray!" : "");
    i = row(y, i, "Interrupts", v, irq_stray_count ? C_WARN : C_TEXT);
    {                                   /* the two busiest lines */
        int a = -1, bb = -1;

        for (int l = 0; l < 32; l++) {
            if (!line_per_s[l])
                continue;
            if (a < 0 || line_per_s[l] > line_per_s[a]) {
                bb = a;
                a = l;
            } else if (bb < 0 || line_per_s[l] > line_per_s[bb]) {
                bb = l;
            }
        }
        if (a >= 0 && bb >= 0)
            snprintf(v, sizeof(v), "#%d %lu, #%d %lu", a, (unsigned long)line_per_s[a],
                     bb, (unsigned long)line_per_s[bb]);
        else if (a >= 0)
            snprintf(v, sizeof(v), "#%d %lu", a, (unsigned long)line_per_s[a]);
        else
            snprintf(v, sizeof(v), "-");
        i = row(y, i, "Busiest", v, C_DIM);
    }
    snprintf(v, sizeof(v), "%lu KB free", (unsigned long)(heap_free_bytes() / 1024));
    i = row(y, i, "Memory", v, C_TEXT);
    if (fi->mounted && fi->free_known)
        snprintf(v, sizeof(v), "%lu MB, %lu free", (unsigned long)(fi->sectors / 2048),
                 (unsigned long)(fi->free_kib / 1024));
    else if (fi->mounted)
        snprintf(v, sizeof(v), "%lu MB", (unsigned long)(fi->sectors / 2048));
    else
        snprintf(v, sizeof(v), "none (%d)", fi->sd_code);
    i = row(y, i, "Card", v, C_TEXT);
    sd_get_stats(&st);
    snprintf(v, sizeof(v), "r%lu w%lu e%lu/%lu", (unsigned long)st.blocks_read,
             (unsigned long)st.blocks_written, (unsigned long)st.retries,
             (unsigned long)st.failures);
    i = row(y, i, "SD blocks", v, st.failures ? C_BAD : C_DIM);
    if (mv > 0)
        snprintf(v, sizeof(v), "%d.%02d V, %d%%%s", mv / 1000, mv % 1000 / 10,
                 battery_percent(mv), charger_present() ? ", chg" : "");
    else
        snprintf(v, sizeof(v), "--%s", charger_present() ? ", chg" : "");
    i = row(y, i, "Battery", v, C_TEXT);
    i = heading(y, i, "Threads");
    for (int n = 0; n < MAX_THREADS && seen[n].t; n++) {
        char label[24];

        snprintf(label, sizeof(label), "%s (%u)", seen[n].t->name, seen[n].t->prio);
        snprintf(v, sizeof(v), "%u.%u%%", seen[n].permille / 10, seen[n].permille % 10);
        i = row(y, i, label, v, C_TEXT);
    }
}

static void tick(screen_t *s)
{
    sample();
}

static void enter(screen_t *s)
{
    first = 0;
    last_total = kernel_cycles();
    last_irqs = irq_count;
    for (int l = 0; l < 32; l++)
        last_line[l] = irq_line_count[l];
    sample();
}

static void key(screen_t *s, const key_event_t *ev)
{
    if (ev->code == KEY_DOWN)
        first++;
    else if (ev->code == KEY_UP && first > 0)
        first--;
    else if (ev->code == KEY_RSOFT)
        ui_pop();
    if (first > 20)
        first = 20;
}

static screen_t sys = {
    .title = "System", .right = "Back",
    .enter = enter, .draw = draw, .key = key, .tick = tick,
};

screen_t *app_system(void)
{
    return &sys;
}
