/* B310E boot loader — what to start, and the menu.
 *
 * boot_early() (hw.c) has already decided the loader stays: the phone was
 * switched on with END, a keypad key was held, or a program asked for
 * something across its reset. Here: chip init, the stock pin table, the
 * panel, the card; then the requested payload, the autoboot entry after
 * its countdown, or the menu: the card's payloads/ (raw images, *.bin) and
 * the stock firmware.
 *
 * The menu looks like B310E OS: its theme, bars, list cards, Montserrat
 * text and icons (os/ui/gfx.c, font_montserrat.c, icons.c, compiled in),
 * with the battery and charger at the top right. A name too long for one
 * line gets a card two rows tall.
 *
 * Holding END for 2 s in the menu switches the phone off (not on the
 * stock firmware's "Hold END" screen).
 *
 * Autoboot: payloads/autoboot.txt names an entry (a file in payloads/, or
 * "stock") and a timeout in seconds. Without the file, or with no entry,
 * the menu just waits. LSOFT ("Auto") makes the selected entry the
 * autoboot entry, or turns autoboot off on the entry that has it. Any key
 * during the countdown stops it.
 *
 * A payload is a raw image linked at the PSRAM base 0x04000000 (B310E OS,
 * Rockbox). It is read there whole and entered at its first word in ARM
 * state, MMU and D-cache off, CP15 V=1.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/ui.h"
#include "boot.h"
#include "ff.h"

#define MAX_ITEMS   24
#define NAME_LEN    64
#define LIST_ROWS   (UI_CONTENT_H / UI_ROW_H)
#define LIST_Y      (UI_TOP + (UI_CONTENT_H - LIST_ROWS * UI_ROW_H) / 2 + 1)
#define CARD_H      (UI_ROW_H - 2)
#define CHUNK       (64u * 1024u)
#define CARD_RADIUS 4                   /* rounder than B310E OS's 2 */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define STOCK_ITEM  "stock"             /* the autoboot name of the stock entry */
#define DEFAULT_TIMEOUT 3

volatile boot_status_t boot_status __attribute__((section(".bss.status")));

typedef struct {
    char name[NAME_LEN];                /* the file in dir; STOCK_ITEM for stock */
    uint32_t size;
} item_t;

static FATFS fs;
static bool card_ok;
static const char *dir = PAYLOAD_DIR;
static item_t items[MAX_ITEMS + 1];     /* the payloads, then the stock entry */
static int nitems;                      /* payloads */
static char auto_entry[NAME_LEN];       /* autoboot: "" off */
static int auto_timeout = DEFAULT_TIMEOUT;
static int countdown = -1;              /* seconds left, -1 none */
static uint32_t status_ms;              /* the battery read last */
static int bat_pct = -1, bat_chg;

static void set_message(const char *s)
{
    str_copy((char *)boot_status.message, s ? s : "", sizeof(boot_status.message));
}

static bool is_stock(const item_t *it)
{
    return !strcasecmp_(it->name, STOCK_ITEM);
}

/* "b310e-os.bin" -> "B310E OS", "rockbox.bin" -> "Rockbox", else the name
 * without .bin */
static void label(const char *name, char *out, size_t n)
{
    size_t len;

    if (!strcasecmp_(name, STOCK_ITEM)) {
        str_copy(out, "Stock firmware", n);
        return;
    }
    if (!strcasecmp_(name, OS_NAME)) {
        str_copy(out, "B310E OS", n);
        return;
    }
    if (!strcasecmp_(name, "rockbox.bin")) {
        str_copy(out, "Rockbox", n);
        return;
    }
    str_copy(out, name, n);
    len = strlen(out);
    if (len > 4 && !strcasecmp_(out + len - 4, ".bin"))
        out[len - 4] = 0;
}

static const icon_t *item_icon(const item_t *it)
{
    if (is_stock(it))
        return &icon_power;
    if (!strcasecmp_(it->name, OS_NAME))
        return &icon_cpu;
    if (!strcasecmp_(it->name, "rockbox.bin"))
        return &icon_music;
    return &icon_file;
}

static void number(uint32_t v, char *out)
{
    char tmp[12];
    int n = 0, i = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        out[i++] = tmp[--n];
    out[i] = 0;
}

static void size_text(uint32_t size, char *out)
{
    uint32_t k = (size + 1023) / 1024;

    if (k >= 1024) {                    /* one decimal: 1.4M */
        uint32_t t = (k * 10 + 512) / 1024;

        number(t / 10, out);
        out += strlen(out);
        *out++ = '.';
        *out++ = (char)('0' + t % 10);
        *out++ = 'M';
        *out = 0;
    } else {
        number(k, out);
        str_copy(out + strlen(out), "K", 2);
    }
}

/* ---- the status bar: as B310E OS draws it -------------------------------- */

static void read_battery(void)
{
    int chg = charge_state();

    bat_chg = chg;
    bat_pct = battery_percent(battery_mv());
    status_ms = ms_now();
}

static void draw_status(void)
{
    char buf[8];
    int sh = gfx_text_height(FONT_SMALL, 1), y = (UI_STATUS_H - sh) / 2;
    int x = LCD_WIDTH - UI_PAD - 17;

    gfx_fill(0, 0, LCD_WIDTH, UI_STATUS_H, C_BAR);
    x = UI_PAD + 2 + gfx_text(UI_PAD + 2, y, "B310E", C_TEXT, FONT_SMALL, 1, 0);
    gfx_text(x + 3, y, BOOT_VERSION_STR, C_DIM, FONT_SMALL, 1, 0);

    x = LCD_WIDTH - UI_PAD - 17;        /* battery: outline, level, cap */
    gfx_rect(x, 2, 15, 7, C_DIM);
    gfx_fill(x + 15, 4, 2, 3, C_DIM);
    if (bat_pct > 0 || bat_chg)
        gfx_fill(x + 2, 4, bat_chg ? 11 : (bat_pct * 11 + 99) / 100, 3,
                 bat_chg ? C_OK : bat_pct <= 15 ? C_BAD : C_TEXT);
    if (bat_chg == CHG_DONE)
        str_copy(buf, "full", sizeof(buf));
    else if (bat_chg)
        str_copy(buf, "chg", sizeof(buf));
    else if (bat_pct >= 0) {
        number((uint32_t)bat_pct, buf);
        str_copy(buf + strlen(buf), "%", 2);
    } else
        str_copy(buf, "--", sizeof(buf));
    x -= gfx_text_width(buf, FONT_SMALL, 1) + 3;
    gfx_text(x, y, buf, C_TEXT, FONT_SMALL, 1, 0);
}

static void bars(const char *title, const char *left, const char *center)
{
    int bh = gfx_text_height(FONT_BODY, 1);
    int y = UI_BOTTOM + (UI_SOFT_H - bh) / 2;

    draw_status();
    gfx_fill(0, UI_STATUS_H, LCD_WIDTH, UI_TITLE_H, C_TITLE);
    gfx_text(UI_PAD + 2, UI_STATUS_H + (UI_TITLE_H - gfx_text_height(FONT_LARGE, 1)) / 2,
             title, C_TEXT, FONT_LARGE, 1, LCD_WIDTH - 2 * UI_PAD - 4);
    gfx_fill(0, UI_BOTTOM, LCD_WIDTH, UI_SOFT_H, C_BAR);
    if (left)
        gfx_text(UI_PAD + 2, y, left, C_TEXT, FONT_BODY, 1, 44);
    if (center) {                       /* the main action, in a pill */
        int w = gfx_text_width(center, FONT_BODY, 1) + 14;

        gfx_round((LCD_WIDTH - w) / 2, UI_BOTTOM + 2, w, UI_SOFT_H - 4,
                  CARD_RADIUS, C_ACCENT);
        gfx_text_centered(y, center, C_SEL_TEXT, FONT_BODY, 1);
    }
}

/* A full-screen state: a name, a line under it, and a bar (max 0: none). */
static void splash(const char *name, const char *line, uint32_t value, uint32_t max)
{
    int y = UI_TOP + UI_CONTENT_H / 2 - 14;

    gfx_fill(0, UI_TOP, LCD_WIDTH, UI_CONTENT_H, C_BG);
    bars("Boot menu", NULL, NULL);
    if (name)
        gfx_text_centered(y, name, C_TEXT, FONT_LARGE, 1);
    if (line)
        gfx_text_centered(y + 18, line, C_DIM, FONT_SMALL, 1);
    if (max)
        gfx_bar(UI_PAD + 12, y + 32, LCD_WIDTH - 2 * UI_PAD - 24, 4,
                (int)(value >> 10), (int)(max >> 10), C_ACCENT);
    lcd_flush();
}

/* ---- the list: one or two rows a card --------------------------------------- */

typedef struct {
    char line1[NAME_LEN], line2[NAME_LEN], detail[12];
    int rows;                           /* 1 or 2 */
} layout_t;

#define CARD_X      UI_PAD
#define TEXT_X      (CARD_X + 4 + ICON_SIZE + 5)

static int card_w(bool bar)
{
    return LCD_WIDTH - 2 * UI_PAD - (bar ? 4 : 0);
}

/* The label on one line with its detail at the right, or split over two
 * lines (at a '-', '_', '.' or ' ' when there is one) with the detail on
 * the second. */
static void lay_out(const item_t *it, bool bar, layout_t *l)
{
    int right = CARD_X + card_w(bar) - 6, full = right - TEXT_X;
    size_t n;

    label(it->name, l->line1, sizeof(l->line1));
    l->line2[0] = 0;
    if (auto_entry[0] && !strcasecmp_(it->name, auto_entry))
        str_copy(l->detail, "auto", sizeof(l->detail));
    else if (is_stock(it))
        l->detail[0] = 0;
    else
        size_text(it->size, l->detail);
    l->rows = 1;
    if (gfx_text_width(l->line1, FONT_BODY, 1) <=
        full - (l->detail[0] ? gfx_text_width(l->detail, FONT_SMALL, 1) + 5 : 0))
        return;
    l->rows = 2;
    n = strlen(l->line1);
    while (n > 1) {                     /* the longest start that fits */
        char c = l->line1[n];

        l->line1[n] = 0;
        if (gfx_text_width(l->line1, FONT_BODY, 1) <= full) {
            l->line1[n] = c;
            break;
        }
        l->line1[n] = c;
        n--;
    }
    for (size_t k = n; k > n / 2; k--) {        /* a nicer break */
        char c = l->line1[k - 1];

        if (c == '-' || c == '_' || c == '.' || c == ' ') {
            n = k;
            break;
        }
    }
    str_copy(l->line2, l->line1 + n, sizeof(l->line2));
    l->line1[n] = 0;
}

static int rows_of(int i, bool bar)
{
    layout_t l;

    lay_out(&items[i], bar, &l);
    return l.rows;
}

static void draw_card(int i, int y, bool sel, bool bar, const layout_t *l)
{
    int w = card_w(bar), h = l->rows * UI_ROW_H - 2, right = CARD_X + w - 6;
    int th = gfx_text_height(FONT_BODY, 1), sh = gfx_text_height(FONT_SMALL, 1);
    uint16_t fg = sel ? C_SEL_TEXT : C_TEXT;
    bool is_auto = auto_entry[0] && !strcasecmp_(items[i].name, auto_entry);
    int dy = y + (l->rows - 1) * UI_ROW_H, dw;

    gfx_round(CARD_X, y, w, h, CARD_RADIUS, sel ? C_ACCENT : C_CARD);
    gfx_icon(CARD_X + 4, y + (CARD_H - ICON_SIZE) / 2, item_icon(&items[i]),
             sel ? C_SEL_TEXT : C_ACCENT);
    dw = l->detail[0] ? gfx_text_width(l->detail, FONT_SMALL, 1) : 0;
    if (dw)
        gfx_text(right - dw, dy + (CARD_H - sh) / 2, l->detail,
                 sel ? C_SEL_TEXT : is_auto ? C_ACCENT : C_DIM, FONT_SMALL, 1, 0);
    if (l->rows == 1) {
        gfx_text(TEXT_X, y + (CARD_H - th) / 2, l->line1, fg, FONT_BODY, 1,
                 right - (dw ? dw + 5 : 0) - TEXT_X);
    } else {
        gfx_text(TEXT_X, y + (CARD_H - th) / 2, l->line1, fg, FONT_BODY, 1,
                 right - TEXT_X);
        gfx_text(TEXT_X, dy + (CARD_H - th) / 2 - 2, l->line2, fg, FONT_BODY, 1,
                 right - (dw ? dw + 5 : 0) - TEXT_X);
    }
}

static int list_rows(void)
{
    return boot_status.message[0] || countdown >= 0 ? LIST_ROWS - 1 : LIST_ROWS;
}

static void draw_menu(int sel, int top, int count)
{
    const char *msg = (const char *)boot_status.message;
    int rows = list_rows(), used = 0, total = 0, before = 0;
    int sh = gfx_text_height(FONT_SMALL, 1);
    bool bar;

    for (int i = 0; i < count; i++) {
        int r = rows_of(i, false);

        total += r;
        if (i < top)
            before += r;
    }
    bar = total > rows;
    gfx_fill(0, UI_TOP, LCD_WIDTH, UI_CONTENT_H, C_BG);
    for (int i = top; i < count; i++) {
        layout_t l;

        lay_out(&items[i], bar, &l);
        if (used + l.rows > rows)
            break;
        draw_card(i, LIST_Y + used * UI_ROW_H, i == sel, bar, &l);
        used += l.rows;
    }
    if (bar) {                          /* a thin scroll indicator */
        int track = rows * UI_ROW_H - 2;
        int h = track * rows / total;

        if (h < 8)
            h = 8;
        gfx_fill(LCD_WIDTH - UI_PAD - 2, LIST_Y, 2, track, C_CARD);
        gfx_fill(LCD_WIDTH - UI_PAD - 2,
                 LIST_Y + (track - h) * before / (total - rows > 0 ? total - rows : 1),
                 2, h, C_DIM);
    }
    if (countdown >= 0 || msg[0]) {     /* the countdown, or what went wrong */
        int my = LIST_Y + rows * UI_ROW_H;
        char text[48];

        if (countdown >= 0) {
            char name[32], n[8];

            label(auto_entry, name, sizeof(name));
            number((uint32_t)countdown, n);
            str_copy(text, name, sizeof(text));
            str_copy(text + strlen(text), " in ", sizeof(text) - strlen(text));
            str_copy(text + strlen(text), n, sizeof(text) - strlen(text));
            str_copy(text + strlen(text), " s", sizeof(text) - strlen(text));
        } else {
            str_copy(text, msg, sizeof(text));
        }
        gfx_round(UI_PAD, my, LCD_WIDTH - 2 * UI_PAD, CARD_H, CARD_RADIUS, C_BAR);
        gfx_icon(UI_PAD + 4, my + (CARD_H - ICON_SIZE) / 2,
                 countdown >= 0 ? &icon_clock : &icon_info,
                 countdown >= 0 ? C_ACCENT : C_BAD);
        gfx_text(UI_PAD + 4 + ICON_SIZE + 5, my + (CARD_H - sh) / 2, text, C_TEXT,
                 FONT_SMALL, 1, LCD_WIDTH - 2 * UI_PAD - ICON_SIZE - 14);
    }
    bars("Boot menu", card_ok ? "Auto" : NULL, "Start");
    lcd_flush();
}

/* ---- autoboot: payloads/autoboot.txt -------------------------------------------- */

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

static void auto_path(char *path, size_t n)
{
    str_copy(path, dir, n);
    str_copy(path + strlen(path), "/" AUTOBOOT_FILE, n - strlen(path));
}

/* entry = <file in payloads/ | stock>, timeout = <seconds>; # comments */
static void auto_load(void)
{
    static FIL f;
    char buf[512], path[32], *line;
    UINT n = 0;

    auto_entry[0] = 0;
    auto_timeout = DEFAULT_TIMEOUT;
    auto_path(path, sizeof(path));
    if (!card_ok || f_open(&f, path, FA_READ) != FR_OK)
        return;
    f_read(&f, buf, sizeof(buf) - 1, &n);
    f_close(&f);
    buf[n] = 0;
    for (line = buf; line && *line; ) {
        char *next = strchr(line, '\n'), *eq, *key, *val;

        if (next)
            *next++ = 0;
        eq = strchr(line, '=');
        if (*trim(line) != '#' && eq) {
            *eq = 0;
            key = trim(line);
            val = trim(eq + 1);
            if (!strcasecmp_(key, "entry")) {
                str_copy(auto_entry, val, sizeof(auto_entry));
            } else if (!strcasecmp_(key, "timeout")) {
                int t = 0;

                while (*val >= '0' && *val <= '9')
                    t = t * 10 + (*val++ - '0');
                auto_timeout = t > 99 ? 99 : t;
            }
        }
        line = next;
    }
}

static const char *auto_save(void)
{
    static FIL f;
    char text[160], n[8], path[32];
    UINT w = 0;

    auto_path(path, sizeof(path));
    str_copy(text, "# B310E boot menu: the entry started by itself after the timeout\n"
                   "# (a file in this folder, or stock). No entry: autoboot off.\n"
                   "entry = ", sizeof(text));
    str_copy(text + strlen(text), auto_entry, sizeof(text) - strlen(text));
    str_copy(text + strlen(text), "\ntimeout = ", sizeof(text) - strlen(text));
    number((uint32_t)auto_timeout, n);
    str_copy(text + strlen(text), n, sizeof(text) - strlen(text));
    str_copy(text + strlen(text), "\n", sizeof(text) - strlen(text));
    if (f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        return "Card not writable";
    if (f_write(&f, text, strlen(text), &w) != FR_OK || w != strlen(text)) {
        f_close(&f);
        return "Card write error";
    }
    return f_close(&f) == FR_OK ? NULL : "Card write error";
}

static int find_item(const char *name, int count)
{
    for (int i = 0; i < count; i++)
        if (!strcasecmp_(items[i].name, name))
            return i;
    return -1;
}

/* ---- starting things -------------------------------------------------------------- */

/* REQ_PATH: the program named in PSRAM, consumed. */
static bool take_request_path(char *path)
{
    volatile uint32_t *b = (volatile uint32_t *)REQ_BLOCK;
    uint32_t sum = b[0];
    int i;

    if (b[0] != REQ_MAGIC)
        return false;
    for (i = 0; i < REQ_PATH_LEN / 4; i++)
        sum += b[1 + i];
    if (b[1 + REQ_PATH_LEN / 4] != ~sum)
        return false;
    for (i = 0; i < REQ_PATH_LEN; i++)
        path[i] = ((volatile char *)&b[1])[i];
    path[REQ_PATH_LEN - 1] = 0;
    b[0] = 0;
    return path[0] != 0;
}

static const char *base_name(const char *path)
{
    const char *s = strchr(path, '/');

    while (s) {
        path = s + 1;
        s = strchr(path, '/');
    }
    return path;
}

/* Returns only on an error, with the reason. */
static const char *start(const char *path)
{
    static FIL f;
    char name[40];
    uint32_t size, done = 0, first;

    boot_status.stage = STAGE_LOAD;
    if (!card_ok)
        return "No card";
    label(base_name(path), name, sizeof(name));
    if (f_open(&f, path, FA_READ) != FR_OK)
        return "Not on the card";
    size = (uint32_t)f_size(&f);
    if (size < 64 || size > LOAD_MAX) {
        f_close(&f);
        return "Not a payload";
    }
    while (done < size) {               /* in chunks, with a progress bar */
        UINT n = 0, want = size - done > CHUNK ? CHUNK : size - done;

        splash(name, "Loading", done, size);
        if (f_read(&f, (void *)(LOAD_ADDR + done), want, &n) != FR_OK || n != want) {
            f_close(&f);
            return "Read error";
        }
        done += n;
    }
    f_close(&f);
    first = REG32(LOAD_ADDR);
    if (first >> 28 != 0xe)             /* an ARM instruction, always */
        return "Not a payload";
    boot_status.loaded = size;
    boot_status.stage = STAGE_ENTER;
    splash(name, "Starting", 0, 0);
    lcd_stop();
    REG32(0x87000000) &= ~1u;           /* keypad: the program sets it up */
    REG32(0x87000010) = 0xfff;
    boot_enter(LOAD_ADDR);
}

static bool missing(const char *path)
{
    static FIL f;

    if (f_open(&f, path, FA_READ) != FR_OK)
        return true;
    f_close(&f);
    return false;
}

/* B310E OS from payloads/, else from the old progs/ */
static const char *start_os(void)
{
    const char *err = start(PAYLOAD_DIR "/" OS_NAME);

    if (card_ok && missing(PAYLOAD_DIR "/" OS_NAME))
        err = start(LEGACY_DIR "/" OS_NAME);
    return err;
}

/* ---- the stock firmware ------------------------------------------------------------ *
 *
 * What the phone showed (2026-11-07):
 *  - power applied always runs code; the stock firmware boots only with
 *    END held and otherwise switches off (or shows its off-charging), and
 *    a later END press is a new power-on, which comes to this loader;
 *  - so the stock must be entered while END is held. A reset with END held
 *    is itself a power-on (v1.6: the request was gone, the menu again); a
 *    jump into the PBL with END held starts it (v1.4, v1.5, faster than a
 *    power-on) but its LCD stayed dark until its own sleep and wake.
 *  - the stock's LCD init (NOR 0x16388) runs the panel's Init only when
 *    bit 2 of its reset status (analog 0x82001320, NOR 0x1a2a0) is set,
 *    which it is until the backlight is switched on (v1.14 to v1.16): the
 *    light goes off the stock's way before the jump (v1.16: the logo);
 *  - its start-up (NOR 0x169f2) reads END again just before the logo
 *    (0x3530a, GPIO 19 = analog EIC channel 3). Let go, it boots only with
 *    a charger (its charging screen), the factory USB-ID voltage (0x22e3e)
 *    or byte 12 of its NV item 22 set (0x2c4c0), which is 0 on this phone:
 *    otherwise it powers off. So END stays held until the logo.
 * "Hold END"; once END has been down 100 ms the jump, with the display as
 * the stock finds it at a power-on (lcd_off()). A keypad key goes back to
 * the menu. */
#define EIC_DATA   0x82001900u

static bool end_held(void)              /* END: analog EIC channel 3 */
{
    return adi_read(EIC_DATA) & 1u << 3;
}

static void stock_prompt(bool waiting)
{
    int y = UI_TOP + UI_CONTENT_H / 2 - 24;

    gfx_fill(0, UI_TOP, LCD_WIDTH, UI_CONTENT_H, C_BG);
    gfx_icon((LCD_WIDTH - ICON_SIZE) / 2, y - 18, &icon_power, C_ACCENT);
    gfx_text_centered(y, "Stock firmware", C_TEXT, FONT_LARGE, 1);
    gfx_text_centered(y + 20, waiting ? "Hold END to start it" : "Starting: keep holding END",
                      waiting ? C_TEXT : C_ACCENT, FONT_BODY, 1);
    gfx_text_centered(y + 36, "until the Samsung logo", C_DIM, FONT_SMALL, 1);
    bars("Boot menu", waiting ? "Back" : NULL, NULL);
    lcd_flush();
}

static void stock(void)
{
    uint32_t since = 0;
    bool down = end_held();             /* held with OK: no prompt */

    boot_status.stage = STAGE_STOCK;
    if (down)                           /* END already held with OK: at once */
        goto go;
    stock_prompt(true);
    for (;;) {
        bool now = end_held();

        if (keypad_poll())
            return;
        if (now && !down) {
            since = ms_now();
        } else if (now && since && ms_now() - since >= 100) {
            break;
        } else if (!now) {
            since = 0;
        }
        down = now;
        ms_wait(10);
    }
go:
    stock_prompt(false);
    lcd_off();
    /* bit 2 of the reset status back first: the stock's LCD init skips the
     * panel's Init without it (hw.c, backlight_off_stock) */
    backlight_off_stock();
    {
        uint32_t t = ms_now();

        while (!(adi_read(ANA_RST_STS) & 1u << 2) && ms_now() - t < 500)
            ;
    }
    if (!(adi_read(ANA_RST_STS) & 1u << 2)) {   /* still clear: no jump */
        backlight_on(60);
        set_message("Stock: display flag stuck");
        return;
    }
    card_close();
    REG32(HWRST_REG) &= ~0xff00u;       /* no reset mode of ours for it */
    stock_jump();
}

static const char *start_item(int i)
{
    char path[NAME_LEN + 12];

    if (is_stock(&items[i])) {
        stock();
        boot_status.stage = STAGE_MENU;
        return NULL;
    }
    str_copy(path, dir, sizeof(path));
    str_copy(path + strlen(path), "/", 2);
    str_copy(path + strlen(path), items[i].name, NAME_LEN);
    return start(path);
}

/* ---- the menu ------------------------------------------------------------------------ */

static bool is_bin(const char *s)
{
    size_t n = strlen(s);

    return n > 4 && !strcasecmp_(s + n - 4, ".bin");
}

/* B310E OS first, then by name */
static bool before(const item_t *a, const item_t *b)
{
    if (!strcasecmp_(b->name, OS_NAME))
        return false;
    if (!strcasecmp_(a->name, OS_NAME))
        return true;
    return strcasecmp_(a->name, b->name) < 0;
}

static void scan(void)
{
    static DIR d;
    static FILINFO fi;

    nitems = 0;
    if (card_ok) {
        if (f_opendir(&d, PAYLOAD_DIR) == FR_OK) {
            dir = PAYLOAD_DIR;
        } else if (f_opendir(&d, LEGACY_DIR) == FR_OK) {
            dir = LEGACY_DIR;
        } else {
            set_message("No payloads/ folder");
            goto stock;
        }
        while (nitems < MAX_ITEMS && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
            item_t it;
            int i;

            if (fi.fattrib & (AM_DIR | AM_HID | AM_SYS) || !is_bin(fi.fname))
                continue;
            str_copy(it.name, fi.fname, NAME_LEN);
            it.size = (uint32_t)fi.fsize;
            for (i = nitems; i > 0 && before(&it, &items[i - 1]); i--)
                items[i] = items[i - 1];
            items[i] = it;
            nitems++;
        }
        f_closedir(&d);
        if (!nitems)
            set_message("No payloads on the card");
    }
stock:
    str_copy(items[nitems].name, STOCK_ITEM, NAME_LEN);
    items[nitems].size = 0;
}

/* Keep sel on screen: top moves until the rows from top to sel fit. */
static int scroll(int sel, int top, int count)
{
    int rows = list_rows(), total = 0;
    bool bar;

    for (int i = 0; i < count; i++)
        total += rows_of(i, false);
    bar = total > rows;
    if (sel < top)
        top = sel;
    for (;;) {
        int used = 0;

        for (int i = top; i <= sel; i++)
            used += rows_of(i, bar);
        if (used <= rows || top == sel)
            return top;
        top++;
    }
}

static void toggle_auto(int sel)
{
    const char *err;
    char text[40];

    if (auto_entry[0] && !strcasecmp_(items[sel].name, auto_entry)) {
        auto_entry[0] = 0;
        str_copy(text, "Autoboot off", sizeof(text));
    } else {
        str_copy(auto_entry, items[sel].name, sizeof(auto_entry));
        if (!auto_timeout)
            auto_timeout = DEFAULT_TIMEOUT;
        str_copy(text, "Autoboot on", sizeof(text));
    }
    err = auto_save();
    set_message(err ? err : text);
}

static void powering_off(void)
{
    splash("B310E", "Powering off", 0, 0);
    ms_wait(300);
    power_off();
    set_message("Power off failed");
}

static void menu(bool autoboot)
{
    int count = nitems + 1, sel = 0, top = 0, key;
    uint32_t t = ms_now(), tick, end_since = 0;
    bool end_armed = false;             /* END let go since: the power-on press */

    boot_status.stage = STAGE_MENU;
    boot_status.items = (uint32_t)count;
    if (autoboot && auto_entry[0]) {
        int i = find_item(auto_entry, count);

        if (i < 0) {
            set_message("Autoboot entry missing");
        } else {
            sel = i;
            countdown = auto_timeout;
        }
    }
    while (ms_now() - t < 250)          /* the key held at power-on */
        keypad_poll();
    read_battery();
    tick = ms_now();
    for (;;) {
        top = scroll(sel, top, count);
        boot_status.sel = (uint32_t)sel;
        boot_status.countdown = (uint32_t)countdown;
        draw_menu(sel, top, count);
        if (countdown == 0) {
            const char *err;

            countdown = -1;
            err = start_item(sel);
            set_message(err);
            boot_status.stage = STAGE_MENU;
            continue;
        }
        for (;;) {                      /* a key, the countdown, the battery */
            ms_wait(10);
            key = keypad_poll();
            if (key)
                break;
            if (countdown > 0 && ms_now() - tick >= 1000) {
                countdown--;
                tick = ms_now();
                break;
            }
            if (ms_now() - status_ms >= 2000) {
                read_battery();
                break;
            }
            if (!end_held()) {          /* END held 2 s: power off */
                end_armed = true;
                end_since = 0;
            } else if (end_armed && !end_since) {
                end_since = ms_now() | 1;
            } else if (end_armed && ms_now() - end_since >= 2000) {
                powering_off();
                end_armed = false;
                break;
            }
        }
        if (!key)
            continue;
        boot_status.keys++;
        if (countdown >= 0) {           /* any key stops the countdown */
            countdown = -1;
            continue;
        }
        switch (key) {
        case KEY_UP: case KEY_2:
            sel = sel ? sel - 1 : count - 1;
            break;
        case KEY_DOWN: case KEY_8:
            sel = sel + 1 < count ? sel + 1 : 0;
            break;
        case KEY_LSOFT:
            if (card_ok)
                toggle_auto(sel);
            break;
        case KEY_CENTER: case KEY_DIAL: case KEY_5:
            keypad_wait_release(1500);
            set_message(start_item(sel));
            boot_status.stage = STAGE_MENU;
            end_armed = false;          /* END may be down from the stock screen */
            break;
        default:
            break;
        }
    }
}

void boot_main(uint32_t early)
{
    uint32_t req = early >> 8;
    bool key = early & EARLY_KEY;
    char path[REQ_PATH_LEN];
    bool have_path = false;
    const char *err = NULL;
    int sd;

    boot_status.magic = BOOT_STATUS_MAGIC;
    boot_status.stage = STAGE_INIT;
    boot_status.early = early;
    boot_status.request = req;
    chip_save();
    chip_init();
    if (req == REQ_PATH)
        have_path = take_request_path(path);
    boot_status.pinmap = (uint32_t)pinmap_apply();
    boot_status.panel = lcd_start();
    splash("B310E", "Reading the card", 0, 0);
    backlight_on(60);
    keypad_open();

    boot_status.stage = STAGE_CARD;
    sd = card_open();
    boot_status.sd_code = (uint32_t)sd;
    boot_status.fs_code = sd ? 0xff : (uint32_t)f_mount(&fs, "", 1);
    card_ok = !sd && !boot_status.fs_code;
    read_battery();                     /* after the card, as B310E OS does */

    if (!key && card_ok) {
        if (req == REQ_OS)
            err = start_os();
        else if (req == REQ_PATH && have_path)
            err = start(path);
        else if (req == REQ_PATH)
            err = "Request lost";
        else if (!req && _start.mode == MODE_PROGRAM && _start.program[0])
            err = start(_start.program);
    }
    scan();
    auto_load();
    if (!card_ok) {
        char m[32], n[8];

        str_copy(m, sd == 2 ? "No card" : sd ? "Card error " : "FAT error ", sizeof(m));
        if (sd != 2) {
            number(sd ? (uint32_t)sd : boot_status.fs_code, n);
            str_copy(m + strlen(m), n, sizeof(m) - strlen(m));
        }
        set_message(m);
    }
    else if (err)
        set_message(err);
    /* autoboot only on a plain power-on: no key held, no request, no error */
    menu(!key && !req && !err);
    for (;;)
        ;
}
