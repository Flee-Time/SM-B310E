/* A Build engine game on B310E OS: what the shared platform layer and sound
 * (os/ports/build) ask of the game (os/ports/duke/duke_game.c, os/ports/sw/sw_game.c).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BUILD_GAME_H
#define BUILD_GAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A key's scancode in the game, and in the menus if another (and a second one there) */
struct build_key { uint8_t game, menu, menu2; };
extern const struct build_key build_keys[128];  /* by KIT_* code */

bool build_in_menu(void);               /* its menus are up: the keys' menu codes */
void build_quit(void);                  /* save its settings and end (never returns) */
void build_each_second(void);           /* from the frame, about once a second */

/* What the game mallocs after the engine's tile cache (the frame, a song, ...): the
 * cache gets the rest of the arena */
extern const size_t build_outside_cache;

/* FX_PlayLoopedAuto's vol is a distance (Duke's sounds.c) rather than a volume */
extern const bool build_looped_vol_is_distance;

void fx_poll(void);                     /* (sound.c) a voice that has ended: the game hears */

#ifdef __cplusplus
}
#endif

#endif
