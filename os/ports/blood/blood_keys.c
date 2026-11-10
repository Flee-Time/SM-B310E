/* Blood on B310E OS: the phone's keys as Blood's (NBlood's keydefaults), and in the
 * menus if other (os/ports/build/build_game.h). Held sideways, the D-pad turned with
 * the screen:
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
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compat.h"
#include "build.h"
#include "types.h"
#include "keyboard.h"
#include "kit.h"
#include "build_game.h"

const struct build_key build_keys[128] = {
    [KIT_UP] = { sc_W, sc_UpArrow, 0 },
    [KIT_DOWN] = { sc_S, sc_DownArrow, 0 },
    [KIT_LEFT] = { sc_LeftArrow, 0, 0 },
    [KIT_RIGHT] = { sc_RightArrow, 0, 0 },
    [KIT_CENTER] = { sc_RightControl, sc_Return, 0 },
    [KIT_LSOFT] = { sc_E, sc_Return, sc_Y },
    [KIT_RSOFT] = { sc_Escape, sc_Escape, sc_N },
    [KIT_DIAL] = { sc_CapsLock, 0, 0 },
    [KIT_END] = { sc_Escape, sc_Escape, 0 },
    [KIT_0 + 1] = { sc_A, 0, 0 },
    [KIT_0 + 3] = { sc_D, 0, 0 },
    [KIT_0 + 4] = { sc_SemiColon, 0, 0 },
    [KIT_0 + 6] = { sc_Quote, 0, 0 },
    [KIT_0 + 5] = { sc_Space, 0, 0 },
    [KIT_0 + 8] = { sc_LeftControl, 0, 0 },
    [KIT_0 + 7] = { sc_OpenBracket, 0, 0 },
    [KIT_0 + 9] = { sc_CloseBracket, 0, 0 },
    [KIT_0] = { sc_Return, 0, 0 },
    [KIT_STAR] = { sc_BackSpace, 0, 0 },
    [KIT_HASH] = { sc_Tab, 0, 0 },
};
