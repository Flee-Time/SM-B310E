/* Chocolate Doom on B310E OS: no SDL (see SDL.h); no game controllers.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CHOCO_SDL_GAMECONTROLLER_H
#define CHOCO_SDL_GAMECONTROLLER_H
#include "SDL_stdinc.h"
typedef enum { SDL_CONTROLLER_BUTTON_MAX = 21 } SDL_GameControllerButton;
typedef int SDL_GameControllerType;
typedef struct SDL_GameController SDL_GameController;
typedef struct SDL_Joystick SDL_Joystick;
#endif
