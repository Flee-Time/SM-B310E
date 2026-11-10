/* Quake on B310E OS: the app's OS side. Its data is id1/ (PAK0.PAK, and PAK1.PAK for
 * the registered game) beside the app (/b310e-os/apps/Games/Quake/id1/) or in
 * /games/quake/id1.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "kit_app.h"

static bool has_data(const char *dir)
{
    FILINFO fi;
    char path[112];

    snprintf(path, sizeof(path), "%s/id1/pak0.pak", dir);
    return f_stat(path, &fi) == FR_OK;
}

static bool prepare(void)
{
    const char *own = app_dir();
    const char *dir = own && strcmp(own, APP_DIR) && has_data(own) ? own
                      : has_data("/games/quake") ? "/games/quake" : NULL;

    if (!dir) {
        kit_fail("Put id1/PAK0.PAK beside this app, or in /games/quake on the card.");
        return false;
    }
    kit_set_dir(dir);
    kit_arg("quake");
    return true;
}

/* (a drop, as Blood's, for now) */
static const icon_t icon = { {
    0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00,
    0x00, 0x00, 0x0f, 0xf0, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x0f, 0xff, 0xff, 0xf0, 0x00,
    0x00, 0xff, 0xff, 0xff, 0xff, 0x00,
    0x00, 0xff, 0xff, 0xff, 0xff, 0x00,
    0x0f, 0xff, 0xff, 0xff, 0xff, 0xf0,
    0x0f, 0xff, 0xff, 0xff, 0xff, 0xf0,
    0x0f, 0xff, 0xff, 0xff, 0xff, 0xf0,
    0x00, 0xff, 0xff, 0xff, 0xff, 0x00,
    0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
} };

const kit_game_t kit_game = {
    .log_name = "quake",
    .stack = 64 * 1024,
    .arena_reserve = 128 * 1024,
    .arena_min = 1024 * 1024,
    .view = { KIT_ROT_90, KIT_FILL, 1, KIT_KEYS_TURNED },
    .prepare = prepare,
};

KIT_APP("Quake", &icon);
