/* Shadow Warrior on B310E OS: what the shared Build layer (os/ports/build/build_game.h)
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
 *   *           turn around
 *   #           the map
 *   END         quit
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>

#include "build.h"
#include "types.h"
#include "keyboard.h"
#include "kit.h"
#include "build_game.h"

extern unsigned char UsingMenus;        /* (menus.c: a BOOL) */
void TerminateGame(void);               /* (game.c) */

const struct build_key build_keys[128] = {
    [KIT_UP] = { sc_UpArrow, 0, 0 },
    [KIT_DOWN] = { sc_DownArrow, 0, 0 },
    [KIT_LEFT] = { sc_LeftArrow, 0, 0 },
    [KIT_RIGHT] = { sc_RightArrow, 0, 0 },
    [KIT_CENTER] = { sc_LeftControl, sc_Return, 0 },
    [KIT_LSOFT] = { sc_Space, sc_Return, sc_Y },
    [KIT_RSOFT] = { sc_Escape, sc_Escape, sc_N },
    [KIT_DIAL] = { sc_CapsLock, 0, 0 },
    [KIT_END] = { sc_Escape, sc_Escape, 0 },
    [KIT_0 + 1] = { sc_Comma, 0, 0 },
    [KIT_0 + 3] = { sc_Period, 0, 0 },
    [KIT_0 + 4] = { sc_SemiColon, 0, 0 },
    [KIT_0 + 6] = { sc_Quote, 0, 0 },
    [KIT_0 + 5] = { sc_A, 0, 0 },
    [KIT_0 + 8] = { sc_Z, 0, 0 },
    [KIT_0 + 7] = { sc_OpenBracket, 0, 0 },
    [KIT_0 + 9] = { sc_CloseBracket, 0, 0 },
    [KIT_0] = { sc_Return, 0, 0 },
    [KIT_STAR] = { sc_BackSpace, 0, 0 },
    [KIT_HASH] = { sc_Tab, 0, 0 },
};

bool build_in_menu(void) { return UsingMenus != 0; }

void build_quit(void)
{
    TerminateGame();                    /* its settings, the engine down */
    exit(0);
}

void build_each_second(void) {}

/* the frame, a song, the sprites' own data (USER, malloc'd as they come): more than Duke.
 * Measured on its first map: 300 KB held back left its heap at least 82 KB free, 380 KB
 * 163 KB; 340 KB keeps about 120 KB of margin for a map with more actors */
const size_t build_outside_cache = 340 * 1024;

const bool build_looped_vol_is_distance = false;
