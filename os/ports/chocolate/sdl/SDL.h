/* Chocolate Doom on B310E OS: no SDL. The few names of SDL's that the
 * game's own files use (types, byte order, the pref path, qsort) are here;
 * everything SDL did is in os/ports/chocolate (i_*.c) and the game kit.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CHOCO_SDL_H
#define CHOCO_SDL_H
#include "SDL_stdinc.h"
#include "SDL_endian.h"
typedef union SDL_Event SDL_Event;      /* i_input.h's prototypes only */
typedef struct SDL_Window SDL_Window;   /* textscreen's txt_sdl.h (Heretic) */
/* i_system.c's: nothing to shut down, no message boxes (the kit shows
 * I_Error's text once the game has stopped) */
#define SDL_Quit() ((void)0)
#define SDL_MESSAGEBOX_ERROR 0
#define SDL_ShowSimpleMessageBox(flags, title, text, window) ((void)0)
#endif
