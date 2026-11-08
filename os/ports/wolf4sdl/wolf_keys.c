/* Wolf4SDL on B310E OS: the phone's keys as the keyboard's Wolf3D knows
 * (sdl_kit.c turns them into scancode events).
 *
 * Keys, with the phone sideways (the D-pad turned with the screen):
 *   D-pad      move and turn, the menus
 *   Center     fire (Ctrl), select (Enter)
 *   LSoft, 5   open doors (Space); yes (Y)
 *   RSoft      the menu, back, no (Esc)
 *   Dial       run on/off (Shift)
 *   8          strafe while held (Alt)
 *   1-4        the knife, the pistol, the machine gun, the chain gun
 *   7, 9       save, load (F2, F3)
 *   END        quit (F10: asks first)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SDL.h"
#include "kit.h"

#define K(code, a, b) [code] = { a, b }

const SDL_Scancode wolf_keys[128][2] = {
    K(KIT_UP, SDL_SCANCODE_UP, SDL_SCANCODE_UNKNOWN),
    K(KIT_DOWN, SDL_SCANCODE_DOWN, SDL_SCANCODE_UNKNOWN),
    K(KIT_LEFT, SDL_SCANCODE_LEFT, SDL_SCANCODE_UNKNOWN),
    K(KIT_RIGHT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UNKNOWN),
    K(KIT_CENTER, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RETURN),
    K(KIT_LSOFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_Y),
    K(KIT_0 + 5, SDL_SCANCODE_SPACE, SDL_SCANCODE_Y),
    K(KIT_RSOFT, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_UNKNOWN),
    K(KIT_DIAL, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_UNKNOWN),
    K(KIT_0 + 8, SDL_SCANCODE_LALT, SDL_SCANCODE_UNKNOWN),
    K(KIT_0 + 1, SDL_SCANCODE_1, SDL_SCANCODE_UNKNOWN),
    K(KIT_0 + 2, SDL_SCANCODE_2, SDL_SCANCODE_UNKNOWN),
    K(KIT_0 + 3, SDL_SCANCODE_3, SDL_SCANCODE_UNKNOWN),
    K(KIT_0 + 4, SDL_SCANCODE_4, SDL_SCANCODE_UNKNOWN),
    K(KIT_0 + 7, SDL_SCANCODE_F2, SDL_SCANCODE_UNKNOWN),
    K(KIT_0 + 9, SDL_SCANCODE_F3, SDL_SCANCODE_UNKNOWN),
    K(KIT_END, SDL_SCANCODE_F10, SDL_SCANCODE_UNKNOWN),
};

const Uint8 wolf_toggles[128] = {
    [KIT_DIAL] = 1,                     /* run: on, off */
};
