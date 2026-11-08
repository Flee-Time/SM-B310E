/* B310E OS — kernel internals shared between the kernel files.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_KERNEL_INTERNAL_H
#define OS_KERNEL_INTERNAL_H

#include "os/kernel.h"

enum { T_READY = 1, T_BLOCKED, T_DEAD };

/* All with interrupts off. */
int k_block(waitq_t *q, uint32_t timeout_ms);   /* the current thread */
void k_wake(thread_t *t, int result);
void k_preempt(void);       /* switch now if a higher thread is ready */
void k_set_prio(thread_t *t, int prio);
void k_mutex_inherit(mutex_t *m, int prio);
int k_mutex_ceiling(thread_t *t);   /* base or inherited priority */
void k_mutex_drop(thread_t *t, mutex_t *m);     /* t ends: the next waiter has it */

#endif
