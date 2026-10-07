/*
 * B310E-OS — include/kernel.h
 *
 * Kernel public API. Aggregates the individual kernel subsystem headers
 * (scheduler, queues, module framework, bump allocator, printk) so that a
 * single `#include "kernel.h"` (via os.h) is enough for app/driver code.
 *
 * The implementation lives in kernel/ — this header is the stable
 * interface later waves (LCD/keypad/USB, integration) compile against.
 */

#ifndef B310E_OS_KERNEL_H
#define B310E_OS_KERNEL_H

#include "sched.h"
#include "queue.h"
#include "module.h"
#include "malloc.h"
#include "printk.h"
#include "irq.h"

#endif /* B310E_OS_KERNEL_H */
