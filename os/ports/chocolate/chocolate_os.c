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

static bool look_in(const char *dir)
{
    FILINFO fi;
    char path[112];

    for (unsigned i = 0; i < ARRAY_SIZE(iwads); i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, iwads[i]);
        if (f_stat(path, &fi) == FR_OK && !(fi.fattrib & AM_DIR)) {
            kit_set_dir(dir);
            kit_arg(LOG);
            kit_arg("-iwad");
            kit_arg(iwads[i]);
            more_args(dir);
            first_config(dir);
            return true;
        }
    }
    return false;
}

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
            kit_set_dir(n ? dir : "/");
            kit_arg(LOG);
            kit_arg("-iwad");
            kit_arg(slash + 1);
            more_args(n ? dir : "/");
            first_config(n ? dir : "/");
            return true;
        }
    }
    if (own && strcmp(own, APP_DIR) && look_in(own))
        return true;
    for (unsigned d = 0; d < ARRAY_SIZE(old_dirs); d++)
        if (look_in(old_dirs[d]))
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
};

KIT_APP(NAME, &icon);
