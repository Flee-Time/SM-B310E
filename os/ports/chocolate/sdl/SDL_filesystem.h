/* Chocolate Doom on B310E OS: no SDL (see SDL.h). No per-user folder:
 * the configuration, the saves and the autoload folder are the game's
 * folder (the kit's, relative paths).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CHOCO_SDL_FILESYSTEM_H
#define CHOCO_SDL_FILESYSTEM_H
#include "SDL_stdinc.h"
#define SDL_GetPrefPath(org, app) ((char *)NULL)
#endif
