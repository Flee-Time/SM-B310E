/* B310E OS — album art (os/media/art.c): the cover of a track, decoded in
 * a low thread to side x side RGB565 pixels (at most 96 x 96).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_ART_H
#define OS_ART_H

#include <stdint.h>
#include "os/media.h"

/* Ask for the cover of path (info: from media_info(), its embedded art).
 * Asking again for the same path and size does nothing. */
void art_request(const char *path, const media_info_t *info, int side);
/* The pixels once decoded, else NULL; *state: 0 still working (or not
 * asked), 1 ready, 2 there is no cover. Valid until the next request. */
const uint16_t *art_get(const char *path, int side, int *state);

#endif
