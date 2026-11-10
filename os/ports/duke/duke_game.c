/* Duke Nukem 3D on B310E OS: what the shared Build layer (os/ports/build/build_game.h)
 * asks of the game.
 *
 * Keys (held sideways, the D-pad turned with the screen):
 *   D-pad       move and turn; the menus
 *   Center      fire; select
 *   LSoft       open, use; yes
 *   RSoft       the menu; back
 *   Dial        run on / off
 *   5, 8        jump, crouch
 *   1, 3        strafe left, right
 *   4, 6        weapon before, after
 *   7, 9        item before, after
 *   0           use the item
 *   *           quick kick
 *   #           the map
 *   END         quit (asks; LSoft: yes)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "duke3d.h"
#include "keyboard.h"
#include "kit.h"
#include "build_game.h"

void keep_effects(void);                /* (premap.c, from prepare.py) */
void gameexit(const char *t);           /* (game.c) */

/* A key's scancode in the game, and in the menus if another */
const struct build_key build_keys[128] = {
    [KIT_UP] = { sc_UpArrow, 0, 0 },
    [KIT_DOWN] = { sc_DownArrow, 0, 0 },
    [KIT_LEFT] = { sc_LeftArrow, 0, 0 },
    [KIT_RIGHT] = { sc_RightArrow, 0, 0 },
    [KIT_CENTER] = { sc_LeftControl, sc_Return, 0 },
    [KIT_LSOFT] = { sc_Space, sc_Return, sc_Y },
    [KIT_RSOFT] = { sc_Escape, sc_Escape, sc_N },
    [KIT_DIAL] = { sc_CapsLock, 0, 0 },
    [KIT_END] = { sc_F10, sc_F10, 0 },
    [KIT_0 + 1] = { sc_Comma, 0, 0 },
    [KIT_0 + 3] = { sc_Period, 0, 0 },
    [KIT_0 + 4] = { sc_SemiColon, 0, 0 },
    [KIT_0 + 6] = { sc_Quote, 0, 0 },
    [KIT_0 + 5] = { sc_A, 0, 0 },
    [KIT_0 + 8] = { sc_Z, 0, 0 },
    [KIT_0 + 7] = { sc_OpenBracket, 0, 0 },
    [KIT_0 + 9] = { sc_CloseBracket, 0, 0 },
    [KIT_0] = { sc_Return, 0, 0 },
    [KIT_STAR] = { sc_Tilde, 0, 0 },
    [KIT_HASH] = { sc_Tab, 0, 0 },
};

bool build_in_menu(void) { return (ps[myconnectindex].gm & MODE_MENU) != 0; }

void build_quit(void) { gameexit(""); }

void build_each_second(void) { keep_effects(); }

/* the frame, the music: about 360 KB; its sounds are in the cache */
const size_t build_outside_cache = 360 * 1024;

/* (its sounds.c gives the distance as the volume, and pan3dsound sets it from then on) */
const bool build_looped_vol_is_distance = true;
