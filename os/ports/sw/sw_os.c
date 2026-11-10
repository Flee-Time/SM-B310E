/* Shadow Warrior on B310E OS: the app's OS side. Its data is SW.GRP, beside the app
 * (/b310e-os/apps/Games/Shadow Warrior/) or in the boot menu's old /games/sw.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "kit_app.h"

static bool has_data(const char *dir)
{
    FILINFO fi;
    char path[112];

    snprintf(path, sizeof(path), "%s/sw.grp", dir);
    return f_stat(path, &fi) == FR_OK;
}

static bool prepare(void)
{
    const char *own = app_dir();
    const char *dir = own && strcmp(own, APP_DIR) && has_data(own) ? own
                      : has_data("/games/sw") ? "/games/sw" : NULL;

    if (!dir) {
        kit_fail("Put SW.GRP beside this app, or in /games/sw on the card.");
        return false;
    }
    kit_set_dir(dir);
    kit_arg("sw");
    return true;
}

/* a shuriken */
static const icon_t icon = { {
    0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x0f, 0xf0, 0x0f, 0xf0, 0x00,
    0xff, 0xff, 0x00, 0x00, 0xff, 0xff,
    0xff, 0xff, 0x00, 0x00, 0xff, 0xff,
    0x00, 0x0f, 0xf0, 0x0f, 0xf0, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
} };

const kit_game_t kit_game = {
    .log_name = "sw",
    .stack = 24 * 1024,                 /* it used 11 KB: the rest is tile cache */
    .arena_reserve = 128 * 1024,
    .arena_min = 1024 * 1024,
    .view = { KIT_ROT_90, KIT_FILL, 1, KIT_KEYS_TURNED },
    .prepare = prepare,
};

KIT_APP("Shadow Warrior", &icon);
