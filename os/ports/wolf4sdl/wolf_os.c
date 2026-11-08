/* Wolf4SDL (Wolfenstein 3D) on B310E OS: the app's OS side. The data
 * (the registered game's *.WL6, GoodTimes/Activision 1.4) goes beside the
 * app (/b310e-os/apps/Wolf3D/); the boot menu's old /games/wolf3d is
 * looked in too. The configuration and the saves go beside the data.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "kit_app.h"

static bool has_data(const char *dir)
{
    FILINFO fi;
    char path[96];

    snprintf(path, sizeof(path), "%s/vswap.wl6", dir);
    return f_stat(path, &fi) == FR_OK;
}

static bool prepare(void)
{
    const char *own = app_dir();
    const char *dir = own && strcmp(own, APP_DIR) && has_data(own) ? own
                      : has_data("/games/wolf3d") ? "/games/wolf3d" : NULL;

    if (!dir) {
        kit_fail("Put Wolfenstein 3D's data (*.WL6) beside wolf3d.app "
                 "(in " APP_DIR "/Wolf3D on the card).");
        return false;
    }
    kit_set_dir(dir);
    kit_arg("wolf3d");
    kit_arg("--res");                   /* its own 320 x 200: the kit scales */
    kit_arg("320");
    kit_arg("200");
    kit_arg("--samplerate");            /* the OPL's work a quarter of 44.1 kHz's, */
    kit_arg("11025");                   /* the digitised sounds (7 kHz) made 1.6 x */
    kit_arg("--configdir");             /* the config and saves: beside the data */
    kit_arg(".");
    return true;
}

/* a helmet */
static const icon_t icon = { {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x0f, 0xaa, 0xaa, 0xf0, 0x00,
    0x00, 0xfa, 0xaa, 0xaa, 0xaf, 0x00,
    0x00, 0xfa, 0xaa, 0xaa, 0xaf, 0x00,
    0x0f, 0xaa, 0xaa, 0xaa, 0xaa, 0xf0,
    0x0f, 0xff, 0xff, 0xff, 0xff, 0xf0,
    0x00, 0xf0, 0x00, 0x00, 0x0f, 0x00,
    0x00, 0xf0, 0xf0, 0x0f, 0x0f, 0x00,
    0x00, 0x0f, 0x00, 0x00, 0xf0, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
} };

const kit_game_t kit_game = {
    .log_name = "wolf3d",
    .stack = 64 * 1024,
    .arena_reserve = 384 * 1024,
    .arena_min = 1536 * 1024,
    .view = { KIT_ROT_90, KIT_FILL, 1, KIT_KEYS_TURNED },
    .prepare = prepare,
};

KIT_APP("Wolf3D", &icon);
