/* The game kit for a game's OS-side file (with the SDK's app.h): what the
 * game tells the kit, and what it may call while it gets ready in the UI
 * thread. A game's whole OS side is then, say:
 *
 *     static bool prepare(void) { kit_set_dir(...); kit_arg(...); return true; }
 *     const kit_game_t kit_game = { "doom", 64 * 1024, 384 * 1024, 2048 * 1024,
 *                                   { KIT_ROT_90, KIT_FIT, 1, KIT_KEYS_TURNED }, prepare };
 *     KIT_APP("Doom", &icon);
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIT_APP_H
#define KIT_APP_H

#include "kit.h"

/* os/tests read these by the symbol kit: frames at +0, error at +4,
 * arena at +164 */
struct kit_state {
    volatile uint32_t frames;           /* shown so far */
    char error[160];                    /* why the game did not start or stopped */
    void *arena;                        /* the game's block (malloc's) */
    size_t arena_size;
};
extern struct kit_state kit;

/* an emulator's own option: its name, its values (NULL ended), the first */
typedef struct {
    const char *name;
    const char *const *values;
    uint8_t def;
} kit_option_t;

typedef struct {
    const char *log_name;               /* the log's "doom: ...", the thread's name */
    size_t stack;                       /* the game's thread's */
    size_t arena_reserve;               /* heap the OS keeps while it runs */
    size_t arena_min;                   /* less than this: "Not enough memory" */
    kit_view_t view;                    /* how frames are shown, at first */
    /* In the UI thread before the game: its folder and arguments; false
     * (after kit_fail) when it cannot start. Or NULL. */
    bool (*prepare)(void);
    /* An emulator (kit_menu.c): its ROMs by extension ({".gb", ".gbc",
     * NULL}) in the app's folder and in rom_dirs (NULL ended, or NULL), a
     * list of them first, and its own options ({NULL} ended, or NULL).
     * rom_args() adds a ROM's arguments (kit_arg) after argv[0]: the ROM's
     * file name (in the game's folder) and the options' values. */
    const char *const *rom_exts;
    const char *const *rom_dirs;
    const kit_option_t *options;
    void (*rom_args)(const char *rom, const uint8_t *values);
    int row_grid;                       /* drawn in tiles this many rows high: 0 if not */
} kit_game_t;
extern const kit_game_t kit_game;

void kit_set_dir(const char *dir);      /* relative paths; default: app_dir() */
void kit_arg(const char *s);            /* argv[0], argv[1] ... (copied) */
void kit_fail(const char *text);        /* shown with Back */
screen_t *kit_launch(void);             /* start the game: its screen */

screen_t *kit_start(void);
screen_t *kit_menu_start(void);         /* kit_menu.c */
void kit_menu_after_game(void);
void kit_stop(void);
extern const app_info_t app_info;

#define KIT_APP(name, icon) APP(name, kit_start, kit_stop, icon)

#endif
