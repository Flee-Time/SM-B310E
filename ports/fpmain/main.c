/* B310E-OS — ports/fpmain/main.c
 *
 * Ported SD-card boot menu for the fpdoom fpmain (fpmenu) app skeleton —
 * the UI matches arch/diag_menu_main.c (menu.bin): 5x7 font, 1-px frame,
 * "B310E BOOT" + battery title row, "OK run  END reboot" footer,
 * full-screen status views, vibrator.
 *
 * MENU: the first entry reboots into the stock NOR firmware (BOOT STOCK),
 * then the card's progs/ (OS-like / single apps — os.bin etc.), then the
 * fpdoom games from fpbin/config.json (the config-driven PORTS section,
 * launched with their args — --dir games/doom1 doom etc.).
 *
 * LAUNCH — the fpdoom readbin mechanism (part2's readbin.c, copied to
 * IRAM 0x40004000): the file is read DIRECTLY to the NOR-boot PSRAM window
 * 0x04000000. Apps linked at 0x14000000 self-relocate there via their
 * appended .rel table; caches are flushed before the jump.
 * NOTE: the stock fpmenu computes the load target as
 * `__image_start & 0xfc000000`, which only yields 0x14000000 on the
 * SC6531E (its ram_addr); on the SC6530 NOR boot it computes 0 — the
 * stock game launch is SC6531E-only. Here the target is explicit.
 * No MEM_REMAP or SMC re-init — the file overwrites the framework's own
 * PSRAM, and the readbin runs from IRAM so it survives.
 *
 * SAFETY: no 0x8c pinmux writes; no USB (the block is unpowered on a card
 * boot — fpdoom LIBC_SDIO=3 guards it out); all waits bounded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "syscode.h"
#include "sdio.h"
#include "microfat.h"
#include "fatfile.h"
#include "launchargs.h"
#include "jsonconf.h"

#define LCD_W 128
#define LCD_H 160

#include "font5x7.h"

#define MENU_MAX_ENTRIES 20u        /* progs/ entries collected          */
#define MENU_MAX_GAMES   JC_TOTAL
#define MENU_MAX_CATS    JC_CATEGORIES
#define MENU_MAX_CAT_ITEMS JC_ITEMS
#define MENU_MAX_VISIBLE 17u        /* rows between title and footer     */
#define READBIN_DST      (0x40000000u + 0x4000u)  /* IRAM copy of the
                                                     readbin loader        */
#define APP_LOAD        0x04000000u /* the framework's OWN PSRAM window
                                       (MEM_REMAP=0 — where fpmain runs).
                                       0x14000000 (the stock's app window)
                                       is the SC6531E's native PSRAM, NOT a
                                       valid SC6530 address — the readbin's
                                       write there stalls the AHB. The app
                                       (os.bin/games, linked at 0x14000000)
                                       self-relocates to 0x04000000 via its
                                       .rel table (diff = -0x10000000). */
#define READBIN_MAX_SIZE 0x4000u   /* IRAM 0x40004000..0x40007fff;
                                       leave room below the stack at 0x9000 */

/* palette (menu.bin identical) */
#define COL_BG    0x0000u           /* black                              */
#define COL_FRAME 0x7befu           /* light gray                         */
#define COL_TITLE 0x07e0u           /* green                              */
#define COL_TEXT  0xffffu           /* white                              */
#define COL_SELBG 0xffffu           /* selected entry background          */
#define COL_SELTX 0x0000u           /* selected entry text                */
#define COL_ERR   0xf800u           /* red                                */
#define COL_OK    0x07e0u           /* green                              */

typedef void (*readbin_t)(unsigned clust, unsigned size, uint8_t *ram,
                          fatdata_t *fatdata);

extern uint8_t __image_start[];     /* part1 link base; relocated at runtime */

typedef struct {
    uint32_t clust, size;
    char name[24];
    char *args;                     /* packed argv (games); NULL otherwise */
    uint8_t kind;                   /* 0 BOOT STOCK, 1 prog, 2 game,
                                       3 category (clust = cat index)      */
} menu_item_t;

typedef struct {
    char name[16];
    int n;
    menu_item_t item[MENU_MAX_CAT_ITEMS];
} menu_cat_t;

static menu_item_t s_item[MENU_MAX_ENTRIES + MENU_MAX_CATS + 1];
static menu_cat_t s_cat[MENU_MAX_CATS];
static int s_ncat;
static int s_nitem;                 /* total root entries (1 + progs + cats) */
static int s_cur_cat = -1;          /* -1 = root list, else s_cat index */

static uint16_t *s_fb;
static char s_line[24];

/* ---- framework-mandated app hooks (called by sys_init) ------------------ */

void lcd_appinit(void)
{
    struct sys_display *disp = &sys_data.display;
    unsigned w = disp->w1, h = disp->h1;

    w -= w % 8;
    h -= h % 8;
    disp->w2 = w;
    disp->h2 = h;
}

void keytrn_init(void)
{
    uint8_t keymap[64];
    int i;

    sys_getkeymap(keymap);
    for (i = 0; i < 64; i++)
        sys_data.keytrn[0][i] = keymap[i];
    /* ignore combinations with the power key */
    for (i = 0; i < 64; i++)
        sys_data.keytrn[1][i] = 0;
}

/* ---- progs/ enumeration -------------------------------------------------- */

static int s_pcount;

static int menu_enum_cb(void *cbdata, fat_entry_t *p, const char *name)
{
    uint32_t attr = p->entry.attr;
    unsigned j = 0;
    char *out;

    (void)cbdata;
    if (attr & (FAT_ATTR_DIR | FAT_ATTR_VOL))
        return 0;
    if (s_pcount >= (int)MENU_MAX_ENTRIES)
        return 1;                   /* list full — stop */

    out = s_item[1 + s_pcount].name;
    /* Keep the dotted/LFN spelling and spaces; truncate the display label. */
    while (j < sizeof(s_item[0].name) - 1 && name[j]) {
        out[j] = name[j];
        j++;
    }
    while (j && out[j - 1] == '.')
        j--;
    if (j == 0)
        out[j++] = '?';
    out[j] = '\0';

    s_item[1 + s_pcount].clust = fat_entry_clust(p);
    s_item[1 + s_pcount].size = p->entry.size;
    s_item[1 + s_pcount].args = NULL;
    s_item[1 + s_pcount].kind = 1;
    s_pcount++;
    return 0;
}

/* ---- JSON games and ROM discovery ------------------------------------- */

static int s_gcount;                /* total games collected (all cats)   */

static void cat_name_from_header(const char *name, char *out, unsigned n)
{
    unsigned i = 0, j = 0;

    while (name[i] == '=' || name[i] == ' ')
        i++;                        /* skip leading = and spaces */
    while (name[i] && name[i] != '=' && j + 1 < n)
        out[j++] = name[i++];
    while (j && out[j - 1] == ' ')
        j--;                        /* strip trailing spaces */
    out[j] = '\0';
}

struct rom_enum_context { int (*emit)(void *, const char *); void *data; };

static int rom_enum_cb(void *opaque, fat_entry_t *entry, const char *name)
{
    struct rom_enum_context *context = opaque;
    if (entry->entry.attr & (FAT_ATTR_DIR | FAT_ATTR_VOL | FAT_ATTR_HID))
        return 0;
    return context->emit(context->data, name);
}

static int rom_scan(void *opaque, const char *directory,
                    int (*emit)(void *, const char *), void *data)
{
    (void)opaque;
    unsigned clust = fat_dir_clust(&fatdata_glob, directory);
    if (!clust) return 0;
    struct rom_enum_context context = { emit, data };
    return fat_enum_name(&fatdata_glob, clust, rom_enum_cb, &context);
}

static int games_init(void)
{
    FILE *fi;
    char *menu, *p;
    int cur = -1;                   /* current category index, -1 = none yet */

    /* JSON is the only configuration format. An absent or invalid file
     * leaves BOOT STOCK and progs/ available. */
    fi = fopen(FPBIN_DIR "config.json", "rb");
    if (!fi) return 0;
    menu = json_parse(fi, 0x10000, rom_scan, NULL);
    fclose(fi);
    if (!menu)
        return -1;

    p = menu;
    while (s_gcount < (int)MENU_MAX_GAMES) {
        uint32_t next = *(uint32_t *)p;
        char *name, *args, *first;
        fat_entry_t *fe;

        if (!next)
            break;
        p += next;                  /* the item: [next][name\0][argv] */
        name = (char *)(p + 4);
        args = name + strlen(name) + 1;

        if (!get_first_arg(args)) {
            if (s_ncat < (int)MENU_MAX_CATS) {
                cat_name_from_header(name, s_cat[s_ncat].name,
                                     sizeof(s_cat[s_ncat].name));
                s_cat[s_ncat].n = 0;
                cur = s_ncat++;
            } else {
                cur = -1;           /* too many cats — drop the rest */
            }
            continue;
        }
        first = get_first_arg(args);
        if (!first)
            continue;
        fe = fat_find_path(&fatdata_glob, first);
        if (!fe || (fe->entry.attr & FAT_ATTR_DIR))
            continue;               /* file missing — skip the item */

        if (cur < 0) {              /* a game before any header — make one */
            if (s_ncat >= (int)MENU_MAX_CATS)
                break;
            strcpy(s_cat[s_ncat].name, "GAMES");
            s_cat[s_ncat].n = 0;
            cur = s_ncat++;
        }
        if (s_cat[cur].n >= (int)MENU_MAX_CAT_ITEMS)
            continue;

        {
            unsigned k = 0;
            while (k < 23 && name[k]) {
                s_cat[cur].item[s_cat[cur].n].name[k] = name[k];
                k++;
            }
            s_cat[cur].item[s_cat[cur].n].name[k] = '\0';
        }
        s_cat[cur].item[s_cat[cur].n].clust = fat_entry_clust(fe);
        s_cat[cur].item[s_cat[cur].n].size = fe->entry.size;
        s_cat[cur].item[s_cat[cur].n].args = args;
        s_cat[cur].item[s_cat[cur].n].kind = 2;
        s_cat[cur].n++;
        s_gcount++;
    }

    /* each non-empty category becomes a root entry (kind 3) */
    s_nitem = 1 + s_pcount;
    for (cur = 0; cur < s_ncat; cur++) {
        if (!s_cat[cur].n)
            continue;
        strcpy(s_item[s_nitem].name, s_cat[cur].name);
        s_item[s_nitem].clust = (uint32_t)cur;
        s_item[s_nitem].size = 0;
        s_item[s_nitem].args = NULL;
        s_item[s_nitem].kind = 3;
        s_nitem++;
    }
    return s_gcount;
}

/* ---- drawing (5x7 font, menu.bin identical) ----------------------------- */

static void text_small(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s) {
        const uint8_t *g;
        int col, row;
        char c = *s++;

        if (c < 0x20 || c > 0x7e)
            c = '?';
        g = font5x7[c - 0x20];
        for (row = 0; row < 8; row++) {     /* clear the full 6x8 cell */
            int yy = y + row;
            if (yy < 0 || yy >= LCD_H) continue;
            for (col = 0; col < 6; col++) {
                int xx = x + col;
                if (xx < 0 || xx >= LCD_W) continue;
                s_fb[yy * LCD_W + xx] = bg;
            }
        }
        for (row = 0; row < 7; row++) {     /* glyph: bit 0 = top */
            int yy = y + row;
            if (yy < 0 || yy >= LCD_H) continue;
            for (col = 0; col < 5; col++) {
                int xx = x + col;
                if (xx < 0 || xx >= LCD_W) continue;
                if (g[col] & (1u << row))
                    s_fb[yy * LCD_W + xx] = fg;
            }
        }
        x += 6;
    }
}

static void fill(uint16_t c)
{
    uint16_t *p = s_fb;
    unsigned n = LCD_W * LCD_H;

    do *p++ = c; while (--n);
}

/* 1-px frame (right border one pixel in — the last column can clip). */
static void frame(void)
{
    unsigned x;

    for (x = 0; x < LCD_W; x++) {
        s_fb[x] = COL_FRAME;
        s_fb[(LCD_H - 1) * LCD_W + x] = COL_FRAME;
    }
    for (x = 1; x < LCD_H - 1; x++) {
        s_fb[x * LCD_W] = COL_FRAME;
        s_fb[x * LCD_W + LCD_W - 2] = COL_FRAME;
    }
}

/* Push the framebuffer to the panel (LCDC DMA + wait for it). */
static void menu_push(void)
{
    sys_start_refresh();
    sys_wait_refresh();
}

/* ---- battery (port of drivers/battery.c — SC6530 ADC, VBAT ch5) --------- */

static uint32_t s_bat_ring[60];
static uint32_t s_bat_ring_idx;
static uint32_t s_bat_ring_n;

#define BAT_ADC_BASE  0x82001680u
#define BAT_ADC_ENABLE (BAT_ADC_BASE + 0x00u)
#define BAT_ADC_CHSEL  (BAT_ADC_BASE + 0x04u)
#define BAT_ADC_START  (BAT_ADC_BASE + 0x54u)
#define BAT_ADC_STATUS (BAT_ADC_BASE + 0x5cu)
#define BAT_ADC_RESULT (BAT_ADC_BASE + 0x4cu)
#define BAT_ANA_BASE   0x82001040u
#define BAT_ANA_E0     0x820010E0u
#define BAT_ANA_E4     0x820010E4u
#define BAT_ANA_A0     0x820010A0u
#define BAT_ANA_B0     0x820010B0u
#define BAT_CONV_BUDGET 1000000u

static uint32_t bat_adc_convert_ch(uint32_t ch)
{
    uint32_t n = BAT_CONV_BUDGET;

    adi_write(BAT_ADC_START, adi_read(BAT_ADC_START) | 1u);
    adi_write(BAT_ADC_CHSEL, adi_read(BAT_ADC_CHSEL) & ~0xFu);
    adi_write(BAT_ADC_CHSEL, adi_read(BAT_ADC_CHSEL) | (ch & 0xFu));
    adi_write(BAT_ADC_CHSEL, adi_read(BAT_ADC_CHSEL) | 0x10u);
    adi_write(BAT_ADC_CHSEL, adi_read(BAT_ADC_CHSEL) | 0x20u);
    adi_write(BAT_ADC_ENABLE, adi_read(BAT_ADC_ENABLE) | 2u);
    while (!(adi_read(BAT_ADC_STATUS) & 1u))
        if (--n == 0) {             /* wedged: reset + report */
            adi_write(BAT_ADC_START, adi_read(BAT_ADC_START) | 0x3ffu);
            return 0xFFFFFFFFu;
        }
    n = adi_read(BAT_ADC_RESULT) & 0x3ffu;
    adi_write(BAT_ADC_START, adi_read(BAT_ADC_START) | 0x3ffu);
    return n;
}

static uint32_t bat_read_raw(void)
{
    uint32_t raw = bat_adc_convert_ch(5u);

    if (raw == 0xFFFFFFFFu) {
        adi_write(BAT_ANA_BASE, 1u);
        adi_write(BAT_ANA_BASE, 2u);
        adi_write(BAT_ANA_E0, 0x20u);
        adi_write(BAT_ANA_E0, 0x10u);
        adi_write(BAT_ANA_E4, 0x10u);
        adi_write(BAT_ANA_A0, 8u);
        DELAY(10);
        adi_write(BAT_ANA_B0, 8u);
        adi_write(BAT_ADC_ENABLE, adi_read(BAT_ADC_ENABLE) | 1u);
        adi_write(BAT_ADC_BASE + 0x48u, 0xE0u);
        adi_write(BAT_ADC_BASE + 0x34u, adi_read(BAT_ADC_BASE + 0x34u) | 0x40u);
        adi_write(BAT_ADC_BASE + 0x2cu, adi_read(BAT_ADC_BASE + 0x2cu) | 0x40u);
        adi_write(BAT_ADC_BASE + 0x30u, adi_read(BAT_ADC_BASE + 0x30u) | 0x40u);
        adi_write(BAT_ADC_BASE + 0x28u, adi_read(BAT_ADC_BASE + 0x28u) | 0x40u);
        adi_write(BAT_ADC_BASE + 0x2cu, (adi_read(BAT_ADC_BASE + 0x2cu) & ~0xFu) | 2u);
        adi_write(BAT_ADC_BASE + 0x34u, (adi_read(BAT_ADC_BASE + 0x34u) & ~0xFu) | 3u);
        /* EIC ch3 re-apply (the framework eic_enable, SC6530 path) */
        adi_write(0x820010e4, 0x20u);
        adi_write(0x820010e0, 0x80u);
        adi_write(0x82001904, adi_read(0x82001904) | (1u << 3));
        raw = bat_adc_convert_ch(5u);
        if (raw == 0xFFFFFFFFu)
            return 0u;              /* wedged even after the recovery */
    }
    return raw;
}

static uint32_t bat_avg(void)
{
    uint32_t raw = bat_read_raw();
    uint32_t i, sum = 0;

    s_bat_ring[s_bat_ring_idx] = raw;
    s_bat_ring_idx = (s_bat_ring_idx + 1u) % 60u;
    if (s_bat_ring_n < 60u)
        s_bat_ring_n++;
    for (i = 0; i < s_bat_ring_n; i++)
        sum += s_bat_ring[i];
    return s_bat_ring_n ? sum / s_bat_ring_n : 0u;
}

static uint32_t bat_mv(uint32_t raw)
{
    return (raw * 4390u + 500u) / 1000u;    /* HW-calibrated 4.39 mV/count */
}

static int bat_pct(uint32_t mv)
{
    if (mv <= 3300u) return 0;
    if (mv >= 4200u) return 100;
    return (int)((mv - 3300u) * 100u / 900u);
}

/* ---- vibrate / reboot (proven B310E register chains) --------------------- */

static void menu_vibrate(void)
{
    adi_write(0x82001154, 0xa1b2);
    adi_write(0x82001244, 0x8300);
    adi_write(0x82001240, 1);
    DELAY(130000);
    adi_write(0x82001240, 0);
    adi_write(0x82001154, 0);
}

static void menu_reboot(void)
{
    MEM4(0x8b000228) &= ~0xff00u;   /* clear HWRST1 (sticky boot flag) */
    menu_vibrate();
    sys_wdg_reset(0);
    for (;;) ;
}

/* ---- UI views ------------------------------------------------------------ */

static void menu_title(void)
{
    uint32_t mv = bat_mv(bat_avg());
    int pct = bat_pct(mv);
    int len;

    text_small(2, 2, s_cur_cat < 0 ? "B310E BOOT" : s_cat[s_cur_cat].name,
               COL_TITLE, COL_BG);
    len = sprintf(s_line, "%u.%02uV %d%%", mv / 1000u,
                  (mv % 1000u) / 10u, pct);
    text_small(LCD_W - 4 - 6 * len, 2, s_line, COL_OK, COL_BG);
}

static void menu_footer(void)
{
    text_small(2, LCD_H - 10,
               s_cur_cat < 0 ? "OK run  END reboot"
                             : "OK run  RSOFT back",
               COL_TEXT, COL_BG);
}

/* 7x6 folder glyph drawn pixel-wise (5x7 font has no folder char). */
static void draw_folder(int x, int y, uint16_t fg, uint16_t bg)
{
    static const uint8_t bmp[6] = {
        0x3e, 0x22, 0x7f, 0x41, 0x41, 0x7f
    };
    int row, col;

    for (row = 0; row < 6; row++) {
        for (col = 0; col < 7; col++) {
            int xx = x + col, yy = y + row;
            if (xx >= 0 && xx < LCD_W && yy >= 0 && yy < LCD_H)
                s_fb[yy * LCD_W + xx] = (bmp[row] >> (6 - col)) & 1
                                            ? fg : bg;
        }
    }
}

static void menu_draw_list(uint32_t sel, uint32_t top, uint32_t n)
{
    unsigned i;

    fill(COL_BG);
    frame();
    menu_title();
    for (i = 0; i < MENU_MAX_VISIBLE && top + i < n; i++) {
        int isel = (top + i == sel);
        const menu_item_t *it = &s_item[top + i];
        const char *nm = it->name;

        text_small(2, 10 + (int)i * 8, isel ? ">" : " ", COL_SELBG, COL_BG);
        if (it->kind == 3) {        /* category: folder icon + name */
            draw_folder(10, 10 + (int)i * 8 + 1,
                        isel ? COL_SELTX : COL_TEXT,
                        isel ? COL_SELBG : COL_BG);
            text_small(18, 10 + (int)i * 8, nm,
                       isel ? COL_SELTX : COL_TEXT,
                       isel ? COL_SELBG : COL_BG);
        } else {
            text_small(10, 10 + (int)i * 8, nm,
                       isel ? COL_SELTX : COL_TEXT,
                       isel ? COL_SELBG : COL_BG);
        }
    }
    menu_footer();
}

/* Draw a category's contents (the sub-list inside a folder). */
static void menu_draw_cat_list(uint32_t sel, uint32_t top)
{
    const menu_cat_t *c = &s_cat[s_cur_cat];
    unsigned i;

    fill(COL_BG);
    frame();
    menu_title();
    for (i = 0; i < MENU_MAX_VISIBLE && top + i < (uint32_t)c->n; i++) {
        int isel = (top + i == sel);
        const menu_item_t *it = &c->item[top + i];

        text_small(2, 10 + (int)i * 8, isel ? ">" : " ", COL_SELBG, COL_BG);
        text_small(10, 10 + (int)i * 8, it->name,
                   isel ? COL_SELTX : COL_TEXT,
                   isel ? COL_SELBG : COL_BG);
    }
    menu_footer();
}

/* Full-screen status view, pushed synchronously (the panel shows it). */
static void menu_status(const char *l1, const char *l2, uint16_t color)
{
    fill(COL_BG);
    frame();
    menu_title();
    text_small(2, LCD_H / 2 - 8, l1, color, COL_BG);
    if (l2)
        text_small(2, LCD_H / 2, l2, COL_TEXT, COL_BG);
    menu_footer();
    menu_push();
}

/* ---- launch (the fpdoom readbin mechanism) ------------------------------- */

static void launch_bin(uint32_t clust, uint32_t size, const char *name)
{
    uint8_t *part2;
    uint32_t p2size;
    uint8_t *dst = (uint8_t *)READBIN_DST;
    uint8_t *ram = (uint8_t *)APP_LOAD;
    uint8_t *readbin;

    /* sdboot places the packed image at APP_LOAD, with part2 first and
     * part1 immediately after it. Read the size from that fixed image
     * header: different compilers produce different part2 lengths.
     * Subtracting a guessed length from __image_start can read below RAM. */
    part2 = (uint8_t *)APP_LOAD;
    p2size = *(uint32_t *)(part2 + 4);
    if (p2size <= 8 || p2size > READBIN_MAX_SIZE || (p2size & 3u) ||
        (uintptr_t)__image_start != APP_LOAD + p2size) {
        menu_status("LOADER ERROR", "Rebuild fpmain.bin", COL_ERR);
        sys_wait_ms(1500);
        return;
    }
    readbin = part2 + 8;            /* the readbin loader (start3 header +8) */
    menu_status("STARTING...", name, COL_OK);
    memcpy(dst, readbin, p2size - 8);
    clean_dcache();                 /* flush the memcpy'd stub + stale I-lines */
    invalidate_icache();
    /* The readbin (from IRAM) reads the file DIRECTLY to 0x04000000 —
     * overwriting our own PSRAM —
     * then flushes and jumps. No remap, no SMC, no LCDC handling. A read
     * failure parks silently inside the readbin (its for(;;)). */
    ((readbin_t)dst)(clust, size, ram, &fatdata_glob);
    for (;;) ;
}

static void launch_item(const menu_item_t *it)
{
    menu_status("READING...", it->name, COL_OK);
    if (it->size > 0x200000u) {     /* 2 MB cap (the framework's limit) */
        menu_status("TOO BIG", "> 2 MB not supported", COL_ERR);
        sys_wait_ms(1500);
        return;
    }
    if (it->kind == 2) {            /* game: pack the config args */
        char *d = (char *)CHIPRAM_ADDR;
        extract_args_t x = { 0, 1, d + 0x1000 };

        if (extract_args(&x, it->args, d + 6))
            *(short *)(d + 4) = (short)x.argc;
        else {
            menu_status("ARGS TOO LONG", "Check config.json", COL_ERR);
            sys_wait_ms(1500);
            return;
        }
    } else {
        *(short *)(CHIPRAM_ADDR + 4) = 0;   /* progs: no args */
    }
    launch_bin(it->clust, it->size, it->name);
}

/* ---- boot menu ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    uint8_t *mem;
    unsigned sel = 0, top = 0;
    int rc, key, type, err = 0;
    uint32_t redraw = 1, pb_prev = 0, pb;

    (void)argc;
    (void)argv;

    mem = malloc(LCD_W * LCD_H * 2 + 31);
    if (!mem) for (;;) ;
    s_fb = (uint16_t *)(((intptr_t)mem + 31) & ~31);
    sys_framebuffer(s_fb);
    sys_start();
    while (sys_event(&key) != EVENT_END)
        ;

    /* entry 0 = BOOT STOCK (reboot) */
    s_item[0].name[0] = '\0';
    strcpy(s_item[0].name, "BOOT STOCK");
    s_item[0].kind = 0;

    /* enumerate progs/ (the card is already mounted — entry.c fat_init) */
    s_pcount = 0;
    rc = (int)fat_dir_clust(&fatdata_glob, "progs");
    if (rc)
        fat_enum_name(&fatdata_glob, rc, menu_enum_cb, 0);

    /* JSON ports and discovered ROMs */
    s_nitem = 1 + s_pcount;
    if (games_init() < 0) {
        menu_status("CONFIG ERROR", "Check config.json", COL_ERR);
        sys_wait_ms(1500);
    }

    if (s_nitem == 1)
        err = 1;                    /* nothing to run except BOOT STOCK */
    menu_vibrate();                 /* feedback: the menu is up */

    for (;;) {
        if (redraw) {
            if (err) {
                menu_status("NOTHING TO RUN", "progs/ or config.json",
                            COL_ERR);
            } else if (s_cur_cat >= 0) {
                menu_draw_cat_list(sel, top);
                menu_push();
            } else {
                menu_draw_list(sel, top, (uint32_t)s_nitem);
                menu_push();
            }
            redraw = 0;
        }
        sys_wait_ms(10);

        for (;;) {
            type = sys_event(&key);
            if (type == EVENT_END)
                break;
            if (type == EVENT_KEYDOWN) {
                switch (key) {
                case 0x04:          /* UP */
                case 0x32:          /* 2 */
                    sel = sel == 0 ? (uint32_t)(s_cur_cat < 0
                                ? s_nitem - 1 : s_cat[s_cur_cat].n - 1)
                                   : sel - 1;
                    if (sel < top)
                        top = sel;
                    redraw = 1;
                    break;
                case 0x05:          /* DOWN */
                case 0x35:          /* 5 */
                    sel = sel + 1 >= (uint32_t)(s_cur_cat < 0
                                ? s_nitem : s_cat[s_cur_cat].n)
                          ? 0 : sel + 1;
                    if (sel >= top + MENU_MAX_VISIBLE)
                        top = sel - (MENU_MAX_VISIBLE - 1);
                    redraw = 1;
                    break;
                case 0x09:          /* RSOFT — back to the root list */
                    if (s_cur_cat >= 0) {
                        s_cur_cat = -1;
                        sel = 0;
                        top = 0;
                        redraw = 1;
                    }
                    break;
                case 0x01:          /* DIAL */
                case 0x08:          /* LSOFT */
                case 0x0d:          /* CENTER */
                    if (err)
                        break;
                    if (s_cur_cat >= 0) {
                        launch_item(&s_cat[s_cur_cat].item[sel]);
                    } else {
                        const menu_item_t *it = &s_item[sel];
                        if (it->kind == 0)
                            menu_reboot();  /* BOOT STOCK — never returns */
                        if (it->kind == 3) {    /* enter the category */
                            s_cur_cat = (int)it->clust;
                            sel = 0;
                            top = 0;
                            redraw = 1;
                            break;
                        }
                        launch_item(it);
                    }
                    redraw = 1;         /* launch failed — back to the list */
                    break;
                }
            }
        }
        /* SC6530 EIC bank; adi_read masks the full address to 0x900. */
        pb = (adi_read(0x82001900) >> 3) & 1u;
        if (pb && !pb_prev)
            menu_reboot();
        pb_prev = pb;
    }
}
