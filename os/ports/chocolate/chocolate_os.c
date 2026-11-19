/* Chocolate Doom, Heretic and Hexen on B310E OS: the app's OS side, built
 * once a game (GAME_DOOM, GAME_HERETIC, GAME_HEXEN). The game kit does the
 * rest (os/ports/kit); this finds the IWAD and says how the game starts.
 *
 * The IWAD: the .wad the app was opened with, else one of the game's
 * names beside the .app when it is in a folder of its own
 * (/b310e-os/apps/Doom/doom.wad, API 2.6), else in the boot menu's old
 * folders (/games/doom1 ...). The configuration and the saves go to the
 * IWAD's folder; args.txt there adds arguments (PWADs, DeHackEd ...).
 *
 * Doom with more than one game in its own folder (DOOM.WAD, DOOM2.WAD,
 * TNT.WAD ..., and a PWAD with a .deh of its name, e.g. sandy.wad and
 * sandy.deh, played on DOOM2.WAD) opens on a list of them; the one picked
 * loads the app again with its path (app_reload), and the store keeps it
 * ("app/Doom/last") to be the one selected next time. A game other than
 * Doom 1 (doom.wad, doom1.wad: their saves stay where they always were) keeps
 * its saves and configuration in a folder of its own beside the WADs, named
 * after its file (doom2/, sandy/): Chocolate Doom names them the same for
 * every game (doomsav0.dsg, default.cfg).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "kit_app.h"

#if defined(GAME_DOOM)
#define NAME "Doom"
#define LOG  "doom"
static const char *const iwads[] = {
    "doom1.wad", "doom.wad", "doom2.wad", "plutonia.wad", "tnt.wad",
    "freedoom1.wad", "freedoom2.wad", "freedm.wad", "chex.wad", "hacx.wad",
};
/* the list's names for them (the same order) */
static const char *const titles[] = {
    "Doom (shareware)", "Doom", "Doom II", "Final Doom: Plutonia", "Final Doom: TNT",
    "Freedoom 1", "Freedoom 2", "FreeDM", "Chex Quest", "HACX",
};
#define ROOT_SAVES 2                    /* iwads[0..1] keep the folder's own saves */
#define PWAD_BASE  "doom2.wad"          /* what a PWAD with a .deh is played on */
static const char *const old_dirs[] = { "/games/doom1", "/games/doom2", "/games/doom" };
/* a horned skull */
static const icon_t icon = { {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x08,
    0xa8, 0x0a, 0xff, 0xff, 0xa0, 0x8a,
    0x0f, 0xff, 0xff, 0xff, 0xff, 0xf0,
    0x0f, 0xff, 0xff, 0xff, 0xff, 0xf0,
    0x0f, 0x00, 0xff, 0xff, 0x00, 0xf0,
    0x0f, 0x00, 0xff, 0xff, 0x00, 0xf0,
    0x0f, 0xff, 0xf0, 0x0f, 0xff, 0xf0,
    0x00, 0xff, 0xff, 0xff, 0xff, 0x00,
    0x00, 0xf0, 0xf0, 0x0f, 0x0f, 0x00,
    0x00, 0x0f, 0xff, 0xff, 0xf0, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
} };
#elif defined(GAME_HERETIC)
#define NAME "Heretic"
#define LOG  "heretic"
static const char *const iwads[] = { "heretic.wad", "heretic1.wad" };
static const char *const old_dirs[] = { "/games/heretic" };
/* a staff with a glowing orb */
static const icon_t icon = { {
    0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x00, 0xfa, 0xaf, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00,
    0x00, 0x00, 0x0a, 0xa0, 0x00, 0x00,
    0x00, 0x00, 0x0a, 0xa0, 0x00, 0x00,
    0x00, 0x00, 0x0a, 0xa0, 0x00, 0x00,
    0x00, 0x00, 0x0a, 0xa0, 0x00, 0x00,
    0x00, 0x00, 0x0a, 0xa0, 0x00, 0x00,
    0x00, 0x00, 0x0a, 0xa0, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00,
} };
#elif defined(GAME_HEXEN)
#define NAME "Hexen"
#define LOG  "hexen"
static const char *const iwads[] = { "hexen.wad" };
static const char *const old_dirs[] = { "/games/hexen" };
/* an axe */
static const icon_t icon = { {
    0x00, 0x00, 0x00, 0xff, 0xf0, 0x00,
    0x00, 0x00, 0x0f, 0xff, 0xff, 0x00,
    0x00, 0x00, 0xff, 0xaf, 0xff, 0x00,
    0x00, 0x00, 0x0a, 0xaf, 0xff, 0x00,
    0x00, 0x00, 0xaa, 0x0f, 0xf0, 0x00,
    0x00, 0x0a, 0xa0, 0x00, 0x00, 0x00,
    0x00, 0xaa, 0x00, 0x00, 0x00, 0x00,
    0x0a, 0xa0, 0x00, 0x00, 0x00, 0x00,
    0xaa, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xa0, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
} };
#else
#error "GAME_DOOM, GAME_HERETIC or GAME_HEXEN"
#endif

/* More arguments for the game, one or more a line, in args.txt beside
 * the WAD (upstream's response file): -file sandy.wad -deh sandy.deh,
 * -warp 1 1, -skill 4, -timedemo demo1 ... */
static void more_args(const char *dir)
{
    FILINFO fi;
    char path[112];

    snprintf(path, sizeof(path), "%s/args.txt", dir);
    if (f_stat(path, &fi) == FR_OK && !(fi.fattrib & AM_DIR))
        kit_arg("@args.txt");
}

/* Doom's first configuration: low detail (160 columns, each drawn twice:
 * the screen shows 160 anyway, two columns a pixel, and it is half the
 * drawing). Its Options menu can set it high; the file keeps that. */
static void first_config(const char *dir)
{
#if defined(GAME_DOOM)
    FIL f;
    UINT n;
    char path[112];
    static const char text[] = "detaillevel 1\n";

    snprintf(path, sizeof(path), "%s/default.cfg", dir);
    if (f_open(&f, path, FA_WRITE | FA_CREATE_NEW) == FR_OK) {
        f_write(&f, text, sizeof(text) - 1, &n);
        f_close(&f);
    }
#else
    (void)dir;
#endif
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static bool same_name(const char *a, const char *b)    /* FAT's: case does not matter */
{
    while (*a && lower(*a) == lower(*b))
        a++, b++;
    return !*a && !*b;
}

static bool is_file(const char *dir, const char *name)
{
    FILINFO fi;
    char path[112];

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return f_stat(path, &fi) == FR_OK && !(fi.fattrib & AM_DIR);
}

/* iwads[]' index of a file name, or -1 */
static int iwad_index(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(iwads); i++)
        if (same_name(name, iwads[i]))
            return (int)i;
    return -1;
}

#if defined(GAME_DOOM)
/* "sandy.wad" -> "sandy" (lower case, at most n - 1 characters) */
static void stem_of(const char *name, char *out, size_t n)
{
    size_t i = 0;

    while (name[i] && name[i] != '.' && i + 1 < n) {
        out[i] = (char)lower(name[i]);
        i++;
    }
    out[i] = 0;
}

/* A PWAD (its first four bytes), not an IWAD */
static bool is_pwad(const char *dir, const char *name)
{
    FIL f;
    UINT n = 0;
    char path[112], magic[4];

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (f_open(&f, path, FA_READ) != FR_OK)
        return false;
    f_read(&f, magic, sizeof(magic), &n);
    f_close(&f);
    return n == 4 && magic[0] == 'P' && magic[1] == 'W' && magic[2] == 'A' && magic[3] == 'D';
}

/* The .deh of a PWAD's name beside it ("sandy.deh"), in out; false if none */
static bool deh_of(const char *dir, const char *wad, char *out, size_t n)
{
    char stem[40];

    stem_of(wad, stem, sizeof(stem));
    snprintf(out, n, "%s.deh", stem);
    return is_file(dir, out);
}
#endif

/* The game in dir: the IWAD wad (its folder's own saves and settings: today's
 * way), or for Doom a PWAD (-merge, with its .deh) on DOOM2.WAD. own: Doom's own
 * folder, where a game other than Doom 1 keeps its saves and settings in a
 * folder of its name. */
static bool start_in(const char *dir, const char *wad, bool own)
{
    int i = iwad_index(wad);
#if defined(GAME_DOOM)
    char sub[40], path[112];
    bool pwad = i < 0 && is_pwad(dir, wad);

    if (pwad && !is_file(dir, PWAD_BASE)) {
        snprintf(path, sizeof(path), "%s needs DOOM2.WAD beside it.", wad);
        kit_fail(path);
        return false;
    }
#endif
    kit_set_dir(dir);
    kit_arg(LOG);
    kit_arg("-iwad");
#if defined(GAME_DOOM)
    if (pwad) {
        kit_arg(PWAD_BASE);
        kit_arg("-merge");              /* not -file: new sprites and flats (Sandy's
                                         * Cities' Keen) stop vanilla's sprite loader */
        kit_arg(wad);
        if (deh_of(dir, wad, path, sizeof(path))) {
            kit_arg("-deh");
            kit_arg(path);
        }
    } else
#endif
        kit_arg(i >= 0 ? iwads[i] : wad);
    more_args(dir);
#if defined(GAME_DOOM)
    if (own && !(i >= 0 && i < ROOT_SAVES)) {
        stem_of(wad, sub, sizeof(sub));
        snprintf(path, sizeof(path), "%s/%s", dir, sub);
        f_mkdir(path);                  /* (there already: fine) */
        kit_arg("-savedir");
        kit_arg(sub);
        snprintf(path, sizeof(path), "%s/default.cfg", sub);
        kit_arg("-config");
        kit_arg(path);
        snprintf(path, sizeof(path), "%s/chocolate-doom.cfg", sub);
        kit_arg("-extraconfig");
        kit_arg(path);
        snprintf(path, sizeof(path), "%s/%s", dir, sub);
        first_config(path);
        return true;
    }
#else
    (void)own;
#endif
    first_config(dir);
    return true;
}

static bool look_in(const char *dir, bool own)
{
    for (unsigned i = 0; i < ARRAY_SIZE(iwads); i++)
        if (is_file(dir, iwads[i]))
            return start_in(dir, iwads[i], own);
    return false;
}

#if defined(GAME_DOOM)
/* ---- the list of games in Doom's own folder ---------------------------------------- */
#define MAX_GAMES 12
#define LAST_KEY  "last"                /* the store's "app/Doom/last": the file picked */

typedef struct {
    char file[40];                      /* as on the card */
    char title[32];
    bool pwad;
} game_t;

static game_t *games;
static list_t game_list;
static char games_dir[96];

static void add_game(const char *file, bool pwad)
{
    game_t *g;
    int i = iwad_index(file);
    char stem[32];

    if (game_list.count >= MAX_GAMES)
        return;
    g = &games[game_list.count++];
    snprintf(g->file, sizeof(g->file), "%s", file);
    g->pwad = pwad;
    if (i >= 0) {
        snprintf(g->title, sizeof(g->title), "%s", titles[i]);
    } else {
        stem_of(file, stem, sizeof(stem));
        if (stem[0] >= 'a' && stem[0] <= 'z')
            stem[0] = (char)(stem[0] - 32);
        snprintf(g->title, sizeof(g->title), "%s", stem);
    }
}

/* The games in dir: the IWADs (in iwads[]' order), then each PWAD that has a
 * .deh of its name, when DOOM2.WAD is there for it */
static void scan_games(const char *dir)
{
    DIR d;
    FILINFO fi;
    char deh[48];

    game_list.count = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(iwads); i++)
        if (is_file(dir, iwads[i]))
            add_game(iwads[i], false);
    if (!is_file(dir, PWAD_BASE) || f_opendir(&d, dir) != FR_OK)
        return;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        const char *dot = strrchr(fi.fname, '.');

        if (fi.fattrib & AM_DIR || !dot || !same_name(dot, ".wad") ||
            iwad_index(fi.fname) >= 0 || strlen(fi.fname) >= sizeof(games[0].file))
            continue;
        if (deh_of(dir, fi.fname, deh, sizeof(deh)) && is_pwad(dir, fi.fname))
            add_game(fi.fname, true);
    }
    f_closedir(&d);
}

static void game_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    snprintf(text, (size_t)tn, "%s", games[i].title);
    snprintf(detail, (size_t)dn, "%s", games[i].pwad ? "Doom II" : "");
}

static void games_draw(screen_t *s)
{
    list_draw(&game_list);
}

/* The one picked: the app again, fresh, with its path (prepare starts it) */
static void play(int i)
{
    char path[112];

    snprintf(path, sizeof(path), "%s/%s", games_dir, games[i].file);
    db_set_str(LAST_KEY, games[i].file);
    if (app_reload(path))
        ui_pop();                       /* the list goes; the app with it, then again */
}

static void games_key(screen_t *s, const key_event_t *ev)
{
    if (!ev->down)
        return;
    if ((ev->code == KEY_CENTER || ev->code == KEY_LSOFT) && game_list.count)
        play(game_list.sel);
    else if (ev->code == KEY_RSOFT)
        ui_pop();
    else
        list_key(&game_list, ev);
}

static screen_t games_screen = {
    .title = NAME, .center = "Play", .right = "Back",
    .draw = games_draw, .key = games_key,
};

/* kit_game.first: the list when Doom's own folder has more than one game and the
 * app was not opened with one; else NULL (prepare, as before) */
static screen_t *first(void)
{
    const char *arg = app_arg(), *own = app_dir();
    char last[40];

    if ((arg && arg[0]) || !own || !strcmp(own, APP_DIR))
        return NULL;
    games = zalloc(MAX_GAMES * sizeof(game_t));
    if (!games)
        return NULL;
    snprintf(games_dir, sizeof(games_dir), "%s", own);
    scan_games(own);
    if (game_list.count < 2)
        return NULL;                    /* (the block goes with the app) */
    game_list.sel = game_list.top = 0;
    game_list.item = game_item;
    if (db_get_str(LAST_KEY, last, sizeof(last), "") > 0)
        for (int i = 0; i < game_list.count; i++)
            if (same_name(games[i].file, last)) {
                game_list.sel = i;
                game_list.top = i > 2 ? i - 2 : 0;
            }
    return &games_screen;
}
#endif

static bool prepare(void)
{
    const char *arg = app_arg(), *own = app_dir();
    FILINFO fi;

    if (arg && arg[0] == '/' && f_stat(arg, &fi) == FR_OK) {
        const char *slash = strrchr(arg, '/');
        char dir[64];
        size_t n = (size_t)(slash - arg);

        if (n < sizeof(dir)) {
            memcpy(dir, arg, n);
            dir[n] = 0;
            /* Doom's own folder: each game's saves its own (as from the list) */
            return start_in(n ? dir : "/", slash + 1, own && strcmp(own, APP_DIR) &&
                            same_name(dir, own));
        }
    }
    if (own && strcmp(own, APP_DIR) && look_in(own, true))
        return true;
    for (unsigned d = 0; d < ARRAY_SIZE(old_dirs); d++)
        if (look_in(old_dirs[d], false))
            return true;
    {
        char text[160];

        snprintf(text, sizeof(text), "Put %s beside %s.app (in %s/%s on the card).",
                 iwads[0], LOG, APP_DIR, NAME);
        kit_fail(text);
    }
    return false;
}

const kit_game_t kit_game = {
    .log_name = LOG,
    .stack = 64 * 1024,
    .arena_reserve = 384 * 1024,
    .arena_min = 1536 * 1024,
    .view = { KIT_ROT_90, KIT_FILL, 1, KIT_KEYS_TURNED },
    .prepare = prepare,
#if defined(GAME_DOOM)
    .first = first,
#endif
};

KIT_APP(NAME, &icon);
