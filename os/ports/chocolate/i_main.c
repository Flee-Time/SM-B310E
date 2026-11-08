/* Chocolate Doom, Heretic and Hexen on B310E OS: main() is the kit's
 * (os/ports/kit): the arguments the app's OS side set, then the game.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>

#include "config.h"
#include "doomtype.h"
#include "m_argv.h"
#include "m_misc.h"

#include "kit.h"

void D_DoomMain(void);

int kit_main(int argc, char **argv)
{
    myargc = argc;
    myargv = malloc(argc * sizeof(char *));
    if (!myargv)
        return 1;
    for (int i = 0; i < argc; i++)
        myargv[i] = M_StringDuplicate(argv[i]);
    M_FindResponseFile();
    M_SetExeDir();
    D_DoomMain();
    return 0;
}
