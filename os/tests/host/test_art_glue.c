/* Host test glue: os/media/art.c in this translation unit, its static
 * decoder entry reached from test_art.c. Its own unit, without <stdio.h>
 * (mingw's stdio defines snprintf inline, which os/lib.h declares).
 *
 * Depends on art.c: decode_at(path, pos, size, flags, side, px, cancel, gen),
 * MAX_SIDE; C_CARD (os/ui.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "../../media/art.c"

bool artt_decode(const char *path, uint32_t pos, uint32_t flags, int side, uint16_t *px)
{
    return decode_at(path, pos, 0, flags, side, px, NULL, 0);
}

int artt_max_side(void)
{
    return MAX_SIDE;
}

uint16_t artt_card(void)
{
    return C_CARD;
}
