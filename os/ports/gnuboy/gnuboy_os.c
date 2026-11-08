/* gnuboy (Game Boy, Game Boy Color) on B310E OS: the app's OS side. The
 * game kit's front end lists the ROMs (.gb, .gbc) beside the app
 * (/b310e-os/apps/Game Boy/) and in the boot menu's old /games/gameboy;
 * each ROM's options are kept beside it. Saves (.sav, the cartridge's
 * battery RAM; .000, a save state) go beside the ROM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "kit_app.h"

static const char *const exts[] = { ".gb", ".gbc", NULL };
static const char *const dirs[] = { "/games/gameboy", NULL };

/* an original Game Boy's four shades (gnuboy's 0xBBGGRR) */
static const char *const colours[] = { "Blue", "Grey", "Green", NULL };
static const char *const shades[] = {
    NULL,                                               /* gnuboy's own */
    "0xffffff,0xaaaaaa,0x555555,0x000000",
    "0x0fbc9b,0x0fac8b,0x306230,0x0f380f",
};
static const kit_option_t options[] = {
    { "Colours", colours, 0 },
    { NULL, NULL, 0 },
};

static void rom_args(const char *rom, const uint8_t *values)
{
    static const char *const pals[] = { "dmg-bgp", "dmg-wndp", "dmg-obp0", "dmg-obp1" };
    char arg[64];

    if (shades[values[0]])
        for (unsigned i = 0; i < ARRAY_SIZE(pals); i++) {
            snprintf(arg, sizeof(arg), "--%s=%s", pals[i], shades[values[0]]);
            kit_arg(arg);
        }
    kit_arg(rom);
}

/* a Game Boy */
static const icon_t icon = { {
    0x00, 0xff, 0xff, 0xff, 0xff, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0x0f, 0x00,
    0x00, 0xf0, 0xaa, 0xaa, 0x0f, 0x00,
    0x00, 0xf0, 0xaa, 0xaa, 0x0f, 0x00,
    0x00, 0xf0, 0xaa, 0xaa, 0x0f, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0x0f, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0x0f, 0x00,
    0x00, 0xf0, 0xf0, 0x00, 0xff, 0x00,
    0x00, 0xff, 0xff, 0x0f, 0x0f, 0x00,
    0x00, 0xf0, 0xf0, 0x00, 0x0f, 0x00,
    0x00, 0xf0, 0x00, 0x00, 0x0f, 0x00,
    0x00, 0xff, 0xff, 0xff, 0xff, 0x00,
} };

const kit_game_t kit_game = {
    .log_name = "gnuboy",
    .stack = 32 * 1024,
    .arena_reserve = 384 * 1024,
    .arena_min = 512 * 1024,
    .view = { KIT_ROT_90, KIT_FILL, 1, KIT_KEYS_TURNED },
    .row_grid = 8,                      /* its tiles: the text keeps every line */
    .rom_exts = exts,
    .rom_dirs = dirs,
    .options = options,
    .rom_args = rom_args,
};

KIT_APP("Game Boy", &icon);
