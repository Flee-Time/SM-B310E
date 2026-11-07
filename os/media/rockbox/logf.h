/* B310E OS shim: Rockbox logf.h; no logf buffer, the calls go away.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef B310E_RB_LOGF_H
#define B310E_RB_LOGF_H
#include <math.h>                       /* its logf(float) before the macro */
#undef logf
#define logf(...) do { } while (0)
#endif
