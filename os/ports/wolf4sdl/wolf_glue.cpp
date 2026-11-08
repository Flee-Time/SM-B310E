/* Wolf4SDL on B310E OS: main() is the kit's: Wolf4SDL's own main, renamed
 * wolf_main, built as C++ like the rest of it. The keys: wolf_keys.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SDL.h"
#include "kit.h"

int wolf_main(int argc, char *argv[]);

/* Left and right: wl_play.c's PollKeyboardMove (prepare.py says why). A
 * turn starts at a third of its speed and is up to speed in 300 ms, so a
 * tap turns a little; held, it is the game's own speed. */
#define TURN_START 33                   /* percent */
#define TURN_RAMP 21                    /* tics */

int wolf_turn(int left, int right, int delta, int tics)
{
    static int held, was;
    int dir = (right ? 1 : 0) - (left ? 1 : 0);

    if (!dir || dir != was)
        held = 0;
    was = dir;
    if (!dir)
        return 0;
    held += tics;
    if (held > TURN_RAMP)
        held = TURN_RAMP;
    return dir * delta * (TURN_START + (100 - TURN_START) * held / TURN_RAMP) / 100;
}

extern "C" int kit_main(int argc, char **argv)
{
    return wolf_main(argc, argv);
}

