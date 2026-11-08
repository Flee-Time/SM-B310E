/* What the Build engine takes from a desktop's C library and newlib-nano does not
 * have. It looks at a directory's entries relative to it: here, always the
 * current one (the game's folder).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define dirfd(d) 0
