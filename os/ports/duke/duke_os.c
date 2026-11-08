/* Duke Nukem 3D on B310E OS: the app's OS side. Its data is DUKE3D.GRP, beside the
 * app (/b310e-os/apps/Games/Duke 3D/) or in the boot menu's old /games/duke3d.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "kit_app.h"

static bool has_data(const char *dir)
{
    FILINFO fi;
    char path[112];

    snprintf(path, sizeof(path), "%s/duke3d.grp", dir);
    return f_stat(path, &fi) == FR_OK;
}

static bool prepare(void)
{
    const char *own = app_dir();
    const char *dir = own && strcmp(own, APP_DIR) && has_data(own) ? own
                      : has_data("/games/duke3d") ? "/games/duke3d" : NULL;

    if (!dir) {
        kit_fail("Put DUKE3D.GRP beside this app, or in /games/duke3d on the card.");
        return false;
    }
    kit_set_dir(dir);
    kit_arg("duke3d");
    return true;
}

/* a nuke's sign */
static const icon_t icon = { {
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x0f, 0xff, 0xff, 0xf0, 0x00,
    0x00, 0xff, 0x0f, 0xf0, 0xff, 0x00,
    0x0f, 0xf0, 0x0f, 0xf0, 0x0f, 0xf0,
    0x0f, 0xf0, 0x0f, 0xf0, 0x0f, 0xf0,
    0x0f, 0xff, 0xf0, 0x0f, 0xff, 0xf0,
    0x0f, 0xff, 0xf0, 0x0f, 0xff, 0xf0,
    0x0f, 0xf0, 0xff, 0xff, 0x0f, 0xf0,
    0x00, 0xff, 0xff, 0xff, 0xff, 0x00,
    0x00, 0x0f, 0xff, 0xff, 0xf0, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
} };

const kit_game_t kit_game = {
    .log_name = "duke",
    .stack = 32 * 1024,                 /* it used 10 KB: the rest is tile cache */
    .arena_reserve = 128 * 1024,
    .arena_min = 1024 * 1024,
    .view = { KIT_ROT_90, KIT_FILL, 1, KIT_KEYS_TURNED },
    .prepare = prepare,
};

KIT_APP("Duke 3D", &icon);
