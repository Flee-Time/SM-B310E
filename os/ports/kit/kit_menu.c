/* The game kit's emulator front end (OS side): the ROMs in the app's folder
 * (and the boot menu's old one) as a list; Center plays one, LSoft opens
 * its options: how the screen is turned and scaled, the keypad, and the
 * emulator's own (kit_game.options). A ROM's options are kept beside it,
 * in <rom name>.opt.
 *
 * A game always starts in a fresh app: picking a ROM loads the app again
 * with its path (app_reload, API 2.7), and a game that ends loads it again
 * with the list ("list:<rom>", the cursor on it). An emulator's variables
 * are then never the last game's.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "kit_app.h"

#define MAX_ROMS 96
#define VIEW_OPTIONS 4

typedef struct {
    char path[96];
    char name[40];                      /* the file's name, less its extension */
} rom_t;

static rom_t *roms;
static list_t list;
static int n_options;                   /* VIEW_OPTIONS + the emulator's */
static uint8_t values[VIEW_OPTIONS + 16];
static int opt_rom;                     /* whose options are open */
static list_t opt_list;
static char game_rom[96];               /* the ROM playing */

static const char *const v_rotate[] = { "Upright", "Sideways", "Upside down",
                                        "Sideways, other way", NULL };
static const char *const v_scale[] = { "Fit", "Fill", "Crop", "1:1", NULL };
static const char *const v_smooth[] = { "Smooth", "Sharp", NULL };
static const char *const v_keys[] = { "Turned with the screen", "As it is", NULL };
static const kit_option_t view_options[VIEW_OPTIONS] = {
    { "Screen", v_rotate, 0 },
    { "Scale", v_scale, 0 },
    { "Pixels", v_smooth, 0 },
    { "Keypad", v_keys, 0 },
};

static const kit_option_t *option(int i)
{
    return i < VIEW_OPTIONS ? &view_options[i] : &kit_game.options[i - VIEW_OPTIONS];
}

static int n_values(int i)
{
    int n = 0;

    while (option(i)->values[n])
        n++;
    return n;
}

/* ---- a ROM's options: <rom less its extension>.opt ------------------------------- */
static void opt_path(const char *rom, char *out, size_t n)
{
    const char *dot = strrchr(rom, '.'), *slash = strrchr(rom, '/');
    size_t len = dot && (!slash || dot > slash) ? (size_t)(dot - rom) : strlen(rom);

    snprintf(out, n, "%.*s.opt", (int)len, rom);
}

static void load_values(const char *rom)
{
    char path[112], text[512], *line, *next;
    FIL f;
    UINT n = 0;

    values[0] = kit_game.view.rotate;
    values[1] = kit_game.view.scale;
    values[2] = kit_game.view.smooth ? 0 : 1;
    values[3] = kit_game.view.keys;
    for (int i = VIEW_OPTIONS; i < n_options; i++)
        values[i] = option(i)->def;
    opt_path(rom, path, sizeof(path));
    if (f_open(&f, path, FA_READ) != FR_OK)
        return;
    f_read(&f, text, sizeof(text) - 1, &n);
    f_close(&f);
    text[n] = 0;
    for (line = text; *line; line = next) {     /* "<name> <value>" a line */
        char *sp, *nl = strchr(line, '\n');
        int v = 0;

        next = nl ? nl + 1 : line + strlen(line);
        if (nl)
            *nl = 0;
        if (!(sp = strrchr(line, ' ')))
            continue;
        *sp++ = 0;
        while (*sp >= '0' && *sp <= '9')
            v = v * 10 + (*sp++ - '0');
        for (int i = 0; i < n_options; i++)
            if (!strcmp(line, option(i)->name) && v < n_values(i))
                values[i] = (uint8_t)v;
    }
}

static void save_values(const char *rom)
{
    char path[112], line[64];
    FIL f;
    UINT n;

    opt_path(rom, path, sizeof(path));
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;
    for (int i = 0; i < n_options; i++) {
        int k = snprintf(line, sizeof(line), "%s %d\n", option(i)->name, values[i]);

        f_write(&f, line, (UINT)k, &n);
    }
    f_close(&f);
}

/* ---- playing: in a fresh app (kit_menu_start with the ROM) -------------------------- */
static void play(const char *rom)
{
    if (app_reload(rom))
        ui_pop();                       /* the list goes; the app with it, then again */
}

static screen_t *start_game(const char *rom)
{
    const char *slash = strrchr(rom, '/');
    char dir[96];
    kit_view_t v;

    snprintf(game_rom, sizeof(game_rom), "%s", rom);
    snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - rom) : 0, rom);
    kit_set_dir(dir[0] ? dir : "/");
    load_values(rom);
    v.rotate = values[0];
    v.scale = values[1];
    v.smooth = values[2] == 0;
    v.keys = values[3];
    kit_set_view(&v);
    kit_arg(kit_game.log_name);
    kit_game.rom_args(slash ? slash + 1 : rom, values + VIEW_OPTIONS);
    return kit_launch();
}

/* the game ended (kit_os.c, in the UI thread): back to the list, fresh */
void kit_menu_after_game(void)
{
    char arg[112];

    snprintf(arg, sizeof(arg), "list:%s", game_rom);
    app_reload(arg);
}

/* ---- the options screen ------------------------------------------------------------- */
static void opt_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    snprintf(text, (size_t)tn, "%s", option(i)->name);
    snprintf(detail, (size_t)dn, "%s", option(i)->values[values[i]]);
}

static void opt_draw(screen_t *s) { list_draw(&opt_list); }

static void opt_key(screen_t *s, const key_event_t *ev)
{
    int i = opt_list.sel, n = n_values(opt_list.sel);

    if (!ev->down)
        return;
    if (ev->code == KEY_RIGHT || ev->code == KEY_CENTER) {
        values[i] = (uint8_t)((values[i] + 1) % n);
        ui_redraw();
    } else if (ev->code == KEY_LEFT) {
        values[i] = (uint8_t)((values[i] + n - 1) % n);
        ui_redraw();
    } else if (ev->code == KEY_LSOFT) {
        save_values(roms[opt_rom].path);
        play(roms[opt_rom].path);
    } else if (ev->code == KEY_RSOFT) {
        save_values(roms[opt_rom].path);
        ui_pop();
    } else {
        list_key(&opt_list, ev);
    }
}

static screen_t opt_screen = {
    .title = "Options", .left = "Play", .center = "Change", .right = "Back",
    .draw = opt_draw, .key = opt_key,
};

/* ---- the list of ROMs ------------------------------------------------------------------ */
static bool is_rom(const char *name)
{
    const char *dot = strrchr(name, '.');

    for (int i = 0; dot && kit_game.rom_exts[i]; i++) {
        const char *a = dot, *b = kit_game.rom_exts[i];

        while (*a && *b && (*a | 0x20) == (*b | 0x20))
            a++, b++;
        if (!*a && !*b)
            return true;
    }
    return false;
}

static void scan_dir(const char *dir)
{
    DIR d;
    FILINFO fi;

    if (f_opendir(&d, dir) != FR_OK)
        return;
    while (list.count < MAX_ROMS && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        rom_t *r;
        const char *dot;
        int i;

        if (fi.fattrib & AM_DIR || !is_rom(fi.fname))
            continue;
        r = &roms[list.count];
        if (snprintf(r->path, sizeof(r->path), "%s/%s", dir, fi.fname) >= (int)sizeof(r->path))
            continue;
        dot = strrchr(fi.fname, '.');
        snprintf(r->name, sizeof(r->name), "%.*s", (int)(dot - fi.fname), fi.fname);
        for (i = list.count; i > 0 && strcmp(roms[i - 1].name, r->name) > 0; i--)
            ;                           /* by name */
        if (i < list.count) {
            rom_t t = *r;

            memmove(&roms[i + 1], &roms[i], (size_t)(list.count - i) * sizeof(rom_t));
            roms[i] = t;
        }
        list.count++;
    }
    f_closedir(&d);
}

static void rom_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    snprintf(text, (size_t)tn, "%s", roms[i].name);
    detail[0] = 0;
}

static void list_draw_(screen_t *s)
{
    if (list.count) {
        list_draw(&list);
        return;
    }
    gfx_text_centered(UI_TOP + 30, "No games yet", C_TEXT, FONT_SMALL, 1);
    {
        char text[80];
        const char *dir = app_dir();

        snprintf(text, sizeof(text), "Put them in %s", dir ? dir : APP_DIR);
        gfx_text_centered(UI_TOP + 46, text, C_DIM, FONT_SMALL, 1);
    }
}

static void list_key_(screen_t *s, const key_event_t *ev)
{
    if (!ev->down)
        return;
    if (ev->code == KEY_CENTER && list.count) {
        play(roms[list.sel].path);
    } else if (ev->code == KEY_LSOFT && list.count) {
        opt_rom = list.sel;
        load_values(roms[opt_rom].path);
        opt_list.count = n_options;
        opt_list.sel = opt_list.top = 0;
        opt_screen.title = roms[opt_rom].name;
        ui_push(&opt_screen);
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    } else {
        list_key(&list, ev);
    }
}

static screen_t list_screen = {
    .left = "Options", .center = "Play", .right = "Back",
    .draw = list_draw_, .key = list_key_,
};

/* kit_start for an emulator: the list, or the ROM the app was loaded with */
screen_t *kit_menu_start(void)
{
    const char *arg = app_arg(), *own = app_dir();

    n_options = VIEW_OPTIONS;
    while (kit_game.options && kit_game.options[n_options - VIEW_OPTIONS].name &&
           n_options < (int)sizeof(values))
        n_options++;
    if (arg && arg[0] == '/')
        return start_game(arg);
    roms = zalloc(MAX_ROMS * sizeof(rom_t));
    if (!roms)
        return NULL;
    list.count = list.sel = list.top = 0;
    list.item = rom_item;
    opt_list.item = opt_item;
    if (own && strcmp(own, APP_DIR))
        scan_dir(own);
    for (int i = 0; kit_game.rom_dirs && kit_game.rom_dirs[i]; i++)
        scan_dir(kit_game.rom_dirs[i]);
    if (arg && arg[0] == 'l' && arg[1] == 'i' && arg[2] == 's' && arg[3] == 't' && arg[4] == ':')
        for (int i = 0; i < list.count; i++)
            if (!strcmp(roms[i].path, arg + 5)) {
                list.sel = i;
                list.top = i > 2 ? i - 2 : 0;
            }
    list_screen.title = app_info.name;
    return &list_screen;
}
