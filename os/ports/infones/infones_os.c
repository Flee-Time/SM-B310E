/* InfoNES (the NES) on B310E OS: the app's OS side. The game kit's front
 * end lists the ROMs (.nes) beside the app (/b310e-os/apps/NES/) and in
 * the boot menu's old /games/nes; each ROM's options are kept beside it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "kit_app.h"

static const char *const exts[] = { ".nes", NULL };
static const char *const dirs[] = { "/games/nes", NULL };

static const char *const edges[] = { "Cropped", "Whole", NULL };
/* A European game runs a fifth too fast at NTSC's rate. Auto: what its name
 * or header says, and NTSC if they say nothing. */
static const char *const tvs[] = { "Auto", "NTSC 60 Hz", "PAL 50 Hz", NULL };
static const kit_option_t options[] = {
    { "Edges", edges, 0 },              /* the 8 lines top and bottom a TV hid */
    { "TV", tvs, 0 },
    { NULL, NULL, 0 },
};

static void rom_args(const char *rom, const uint8_t *values)
{
    kit_arg(values[0] ? "--whole" : "--crop");
    if (values[1])
        kit_arg(values[1] == 1 ? "--ntsc" : "--pal");
    kit_arg(rom);
}

/* a NES pad */
static const icon_t icon = { {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0f, 0xff, 0xff, 0xff, 0xff, 0xf0,
    0xf0, 0x00, 0x00, 0x00, 0x00, 0x0f,
    0xf0, 0xf0, 0x00, 0x00, 0x00, 0x0f,
    0xff, 0xff, 0x0a, 0xa0, 0xa0, 0xaf,
    0xf0, 0xf0, 0x00, 0x00, 0x00, 0x0f,
    0xf0, 0x00, 0x00, 0x00, 0x00, 0x0f,
    0x0f, 0xff, 0xff, 0xff, 0xff, 0xf0,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
} };

const kit_game_t kit_game = {
    .log_name = "infones",
    .stack = 32 * 1024,
    .arena_reserve = 384 * 1024,
    .arena_min = 512 * 1024,
    .view = { KIT_ROT_90, KIT_FIT, 1, KIT_KEYS_TURNED },
    .rom_exts = exts,
    .rom_dirs = dirs,
    .options = options,
    .rom_args = rom_args,
};

KIT_APP("NES", &icon);
