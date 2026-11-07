/* B310E OS shim: <errno.h> with a plain errno (os/media/codecs/rt.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef B310E_RB_ERRNO_H
#define B310E_RB_ERRNO_H
extern int errno;
#define EINVAL 22
#define ENOMEM 12
#define EIO 5
#define ERANGE 34
#define EFBIG 27
#endif
