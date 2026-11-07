/* B310E OS — System: pages of live figures, Left/Right between them.
 *
 *   System     uptime, CPU, memory, battery at a glance, the load graph
 *   CPU        load and clock graphs, interrupts per second, busiest lines
 *   Threads    every thread's share of the CPU (Up/Down scroll)
 *   Storage    card, SD blocks read and written a second (graph), heap
 *   Power      battery, charger, backlight, LCD transfers and rows sent
 *
 * The figures that move quickly are graphs over the last 30 s, sampled
 * every half second while the screen is open.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/status.h"
#include "os/ui.h"

#define MAX_THREADS 24
#define LINE_H      11
#define HIST        60                  /* samples: 30 s at 2 a second */
#define SAMPLE_MS   500

enum { P_OVERVIEW, P_CPU, P_THREADS, P_STORAGE, P_POWER, PAGES };
static const char *const page_names[PAGES] = {
    "System", "CPU", "Threads", "Storage", "Power",
};

static struct {
    thread_t *t;
    uint32_t last;
    uint16_t permille;
} seen[MAX_THREADS];

/* the histories: one ring index for all */
static uint16_t h_load[HIST], h_mhz[HIST], h_irq[HIST], h_sd[HIST], h_heap[HIST], h_lcd[HIST];
static int head, filled;

static uint32_t last_total, last_irqs, irqs_per_s, load_pm;
static uint32_t last_line[32], line_per_s[32];
static uint32_t last_rd, last_wr, rd_per_s, wr_per_s;
static uint32_t last_rows, last_kicks, rows_per_s, kicks_per_s;
static int page, first;                 /* first: the Threads page's scroll */

extern volatile uint32_t lcd_kicks, lcd_rows_sent;

static uint32_t per_s(uint32_t d, uint32_t span)
{
    return span ? (uint32_t)((uint64_t)d * 26000000u / span) : 0;
}

static void sample(void)
{
    uint32_t total = kernel_cycles(), span = total - last_total;
    uint32_t s = arch_irq_save(), idle = 0, sum = 0;
    sd_stats_t st;
    int n = 0;

    for (thread_t *t = thread_first(); t && n < MAX_THREADS; t = thread_next(t), n++) {
        uint32_t c = t->cycles;

        if (seen[n].t != t)
            seen[n].last = c;
        seen[n].permille = span >= 1000 ? (uint16_t)((c - seen[n].last) / (span / 1000)) : 0;
        if (!strcmp(t->name, "idle"))
            idle = seen[n].permille;
        sum += seen[n].permille;
        seen[n].t = t;
        seen[n].last = c;
    }
    for (; n < MAX_THREADS; n++)
        seen[n].t = NULL;
    arch_irq_restore(s);

    /* the threads' shares add up to the whole span; a window where they do
     * not (the one across a wake from deep idle, whose sleep the idle
     * thread is credited with later) keeps the last load, not a 100% spike */
    if (sum >= 800 && sum <= 1200)
        load_pm = idle < 1000 ? 1000 - idle : 0;
    irqs_per_s = per_s(irq_count - last_irqs, span);
    last_irqs = irq_count;
    for (int i = 0; i < 32; i++) {
        uint32_t c = irq_line_count[i];

        line_per_s[i] = per_s(c - last_line[i], span);
        last_line[i] = c;
    }
    sd_get_stats(&st);
    rd_per_s = per_s(st.blocks_read - last_rd, span);
    wr_per_s = per_s(st.blocks_written - last_wr, span);
    last_rd = st.blocks_read;
    last_wr = st.blocks_written;
    rows_per_s = per_s(lcd_rows_sent - last_rows, span);
    kicks_per_s = per_s(lcd_kicks - last_kicks, span);
    last_rows = lcd_rows_sent;
    last_kicks = lcd_kicks;
    last_total = total;
}

static void record(void)
{
    uint32_t sd = (rd_per_s + wr_per_s) / 2;        /* KB a second */

    h_load[head] = (uint16_t)load_pm;
    h_mhz[head] = (uint16_t)clock_mhz();
    h_irq[head] = (uint16_t)(irqs_per_s > 65535 ? 65535 : irqs_per_s);
    h_sd[head] = (uint16_t)(sd > 65535 ? 65535 : sd);
    h_heap[head] = (uint16_t)(heap_free_bytes() / 1024);
    h_lcd[head] = (uint16_t)(rows_per_s > 65535 ? 65535 : rows_per_s);
    head = (head + 1) % HIST;
    if (filled < HIST)
        filled++;
}

/* ---- drawing helpers ---------------------------------------------------- */

/* one row: a dim label at the left, the value at the right (the label
 * gives way: it is cut with ".." before it reaches the value) */
static void row(int y, const char *label, const char *value, uint16_t c)
{
    ui_kv(UI_PAD + 2, y, LCD_WIDTH - 2 * UI_PAD - 4, label, value, c);
}

/* a graph with its caption and current value above it */
static int graph(int y, int h, const char *caption, const char *value,
                 const uint16_t *hist, int max, uint16_t c)
{
    row(y, caption, value, C_TEXT);
    gfx_graph(UI_PAD, y + 10, LCD_WIDTH - 2 * UI_PAD, h, hist, HIST, head, max, c);
    return y + 10 + h + 4;
}

static void pct(char *v, int n, uint32_t pm)
{
    snprintf(v, (size_t)n, "%lu.%lu%%", (unsigned long)(pm / 10), (unsigned long)(pm % 10));
}

/* the page dots under the title */
static void dots(void)
{
    int w = PAGES * 6 - 2, x = (LCD_WIDTH - w) / 2;

    for (int i = 0; i < PAGES; i++)
        gfx_round(x + i * 6, UI_TOP + 1, 4, 3, 1, i == page ? C_ACCENT : C_CARD_HI);
}

/* ---- pages -------------------------------------------------------------- */

static void draw_overview(void)
{
    char v[32];
    uint32_t t = kernel_ticks();
    int mv = (int)os_status.battery_mv, y = UI_TOP + 7;

    snprintf(v, sizeof(v), "%luh %02lum %02lus", (unsigned long)(t / 3600000),
             (unsigned long)(t / 60000 % 60), (unsigned long)(t / 1000 % 60));
    row(y, "Uptime", v, C_TEXT);
    y += LINE_H;
    snprintf(v, sizeof(v), "%lu KB free", (unsigned long)(heap_free_bytes() / 1024));
    row(y, "Memory", v, C_TEXT);
    y += LINE_H;
    if (mv > 0)
        snprintf(v, sizeof(v), "%d.%02d V, %d%%", mv / 1000, mv % 1000 / 10, battery_percent(mv));
    else
        snprintf(v, sizeof(v), "--");
    row(y, "Battery", v, C_TEXT);
    y += LINE_H + 2;
    {
        char l[16];

        pct(l, sizeof(l), load_pm);
        snprintf(v, sizeof(v), "%s at %d MHz", l, clock_mhz());
    }
    graph(y, UI_BOTTOM - y - 14, "CPU", v, h_load, 1000, load_pm > 800 ? C_WARN : C_ACCENT);
}

static void draw_cpu(void)
{
    char v[32];
    int y = UI_TOP + 7, a = -1, b = -1;

    pct(v, sizeof(v), load_pm);
    y = graph(y, 24, "Load", v, h_load, 1000, load_pm > 800 ? C_WARN : C_ACCENT);
    snprintf(v, sizeof(v), "%d MHz", clock_mhz());
    y = graph(y, 16, "Clock", v, h_mhz, 208, C_OK);
    snprintf(v, sizeof(v), "%lu/s%s", (unsigned long)irqs_per_s, irq_stray_count ? " stray!" : "");
    y = graph(y, 16, "Interrupts", v, h_irq, 0, irq_stray_count ? C_BAD : C_TEXT);
    for (int l = 0; l < 32; l++) {             /* the two busiest lines */
        if (!line_per_s[l])
            continue;
        if (a < 0 || line_per_s[l] > line_per_s[a]) {
            b = a;
            a = l;
        } else if (b < 0 || line_per_s[l] > line_per_s[b]) {
            b = l;
        }
    }
    if (a >= 0 && b >= 0)
        snprintf(v, sizeof(v), "#%d %lu, #%d %lu", a, (unsigned long)line_per_s[a], b,
                 (unsigned long)line_per_s[b]);
    else if (a >= 0)
        snprintf(v, sizeof(v), "#%d %lu", a, (unsigned long)line_per_s[a]);
    else
        snprintf(v, sizeof(v), "-");
    row(y, "Busiest", v, C_DIM);
}

static void draw_threads(void)
{
    int y = UI_TOP + 7, rows = (UI_BOTTOM - y) / LINE_H, n = 0;

    while (n < MAX_THREADS && seen[n].t)
        n++;
    if (first > n - rows)
        first = n - rows > 0 ? n - rows : 0;
    for (int i = first; i < n && i < first + rows; i++, y += LINE_H) {
        char label[24], v[12];
        int bw = 24;

        snprintf(label, sizeof(label), "%s (%u)", seen[i].t->name, seen[i].t->prio);
        pct(v, sizeof(v), seen[i].permille);
        gfx_text(UI_PAD + 2, y, label, C_TEXT, FONT_SMALL, 1, 64);
        gfx_bar(LCD_WIDTH - UI_PAD - 2 - 32 - bw, y + 1, bw, 5, seen[i].permille, 1000,
                seen[i].permille > 500 ? C_WARN : C_ACCENT);
        gfx_text(LCD_WIDTH - UI_PAD - 2 - gfx_text_width(v, FONT_SMALL, 1), y, v, C_DIM,
                 FONT_SMALL, 1, 0);
    }
    if (n > rows) {                     /* where in the list */
        int track = UI_BOTTOM - UI_TOP - 10;

        gfx_fill(LCD_WIDTH - 2, UI_TOP + 6 + track * first / n, 1, track * rows / n, C_DIM);
    }
}

static void draw_storage(void)
{
    char v[32];
    const fs_info_t *fi = fs_info();
    sd_stats_t st;
    int y = UI_TOP + 7;

    if (fi->mounted && fi->free_known)
        snprintf(v, sizeof(v), "%lu MB, %lu free", (unsigned long)(fi->sectors / 2048),
                 (unsigned long)(fi->free_kib / 1024));
    else if (fi->mounted)
        snprintf(v, sizeof(v), "%lu MB", (unsigned long)(fi->sectors / 2048));
    else
        snprintf(v, sizeof(v), "none (%d)", fi->sd_code);
    row(y, "Card", v, C_TEXT);
    y += LINE_H;
    sd_get_stats(&st);
    snprintf(v, sizeof(v), "%lu retries, %lu failed", (unsigned long)st.retries,
             (unsigned long)st.failures);
    row(y, "Errors", v, st.failures ? C_BAD : C_DIM);
    y += LINE_H + 2;
    snprintf(v, sizeof(v), "r %lu w %lu KB/s", (unsigned long)(rd_per_s / 2),
             (unsigned long)(wr_per_s / 2));
    y = graph(y, 22, "Card I/O", v, h_sd, 0, C_ACCENT);
    snprintf(v, sizeof(v), "%lu KB free", (unsigned long)(heap_free_bytes() / 1024));
    graph(y, UI_BOTTOM - y - 14, "Heap", v, h_heap, 0, C_OK);
}

static void draw_power(void)
{
    char v[32];
    int mv = (int)os_status.battery_mv, y = UI_TOP + 7, st = charge_state();

    if (mv > 0)
        snprintf(v, sizeof(v), "%d.%02d V", mv / 1000, mv % 1000 / 10);
    else
        snprintf(v, sizeof(v), "--");
    row(y, "Battery", v, C_TEXT);
    y += LINE_H;
    gfx_bar(UI_PAD + 2, y, LCD_WIDTH - 2 * UI_PAD - 4, 6, battery_percent(mv), 100,
            battery_percent(mv) < 15 ? C_BAD : C_OK);
    y += 10;
    row(y, "Charger", st == CHARGE_CHARGING ? "charging" : st == CHARGE_DONE ? "charged" :
        charger_present() ? "plugged" : "unplugged", C_TEXT);
    y += LINE_H;
    snprintf(v, sizeof(v), "%d%%, %d s", ui_settings.brightness, ui_settings.screen_timeout_s);
    row(y, "Backlight", v, C_TEXT);
    y += LINE_H;
    snprintf(v, sizeof(v), "%s", ui_settings.deep_idle ? "on" : "off");
    row(y, "Deep idle", v, C_TEXT);
    y += LINE_H + 2;
    snprintf(v, sizeof(v), "%lu rows/s", (unsigned long)rows_per_s);
    graph(y, UI_BOTTOM - y - 14, "Display", v, h_lcd, 0, C_ACCENT);
}

static void draw(screen_t *s)
{
    s->title = page_names[page];
    dots();
    switch (page) {
    case P_OVERVIEW:
        draw_overview();
        break;
    case P_CPU:
        draw_cpu();
        break;
    case P_THREADS:
        draw_threads();
        break;
    case P_STORAGE:
        draw_storage();
        break;
    case P_POWER:
        draw_power();
        break;
    }
}

static void tick(screen_t *s)
{
    sample();
    record();
}

static void enter(screen_t *s)
{
    sd_stats_t st;

    first = 0;
    last_total = kernel_cycles();
    last_irqs = irq_count;
    for (int l = 0; l < 32; l++)
        last_line[l] = irq_line_count[l];
    sd_get_stats(&st);
    last_rd = st.blocks_read;
    last_wr = st.blocks_written;
    last_rows = lcd_rows_sent;
    last_kicks = lcd_kicks;
    memset(h_load, 0, sizeof(h_load));
    memset(h_mhz, 0, sizeof(h_mhz));
    memset(h_irq, 0, sizeof(h_irq));
    memset(h_sd, 0, sizeof(h_sd));
    memset(h_lcd, 0, sizeof(h_lcd));
    for (int i = 0; i < HIST; i++)      /* the heap has a level: start there */
        h_heap[i] = (uint16_t)(heap_free_bytes() / 1024);
    head = filled = 0;
    sample();
}

static void key(screen_t *s, const key_event_t *ev)
{
    switch (ev->code) {
    case KEY_LEFT:
        page = (page + PAGES - 1) % PAGES;
        first = 0;
        break;
    case KEY_RIGHT:
        page = (page + 1) % PAGES;
        first = 0;
        break;
    case KEY_DOWN:
        first++;
        break;
    case KEY_UP:
        if (first > 0)
            first--;
        break;
    case KEY_RSOFT:
        ui_pop();
        break;
    }
}

static screen_t sys = {
    .title = "System", .right = "Back",
    .enter = enter, .draw = draw, .key = key, .tick = tick, .tick_ms = SAMPLE_MS,
};

screen_t *app_system(void)
{
    return &sys;
}
