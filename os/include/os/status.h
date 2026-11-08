/* B310E OS — a fixed status block for tests (QEMU reads it through the ELF
 * symbol `os_status`). Field order is the tests' contract: append only.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OS_STATUS_H
#define OS_STATUS_H

#include <stdint.h>

#define OS_MAGIC 0x534f3342u        /* "B3OS" */

typedef struct {
    uint32_t magic;
    uint32_t ticks;
    uint32_t idle_permille;
    uint32_t selftest_done;         /* bits that ran */
    uint32_t selftest_pass;         /* bits that passed */
    uint32_t pi_wait_ms;            /* high thread's wait behind the mutex */
    uint32_t sem_rounds_per_s;
    uint32_t keys;
    uint32_t last_key;              /* code | down << 8 */
    uint32_t worker_loops;
    uint32_t heap_free;
    uint32_t stray_irqs;
    uint32_t cycles;                /* all accounted cycles */
    uint32_t idle_cycles;
    uint32_t sd_code;               /* sd_init(): 0 ok, else the failed step */
    uint32_t fs_mounted;
    uint32_t fs_free_kib;
    uint32_t sd_retries;
    uint32_t sd_failures;
    uint32_t fs_write_kib_per_s;
    uint32_t fs_read_kib_per_s;
    uint32_t ui_depth;              /* screens on the stack */
    const char *ui_title;           /* the top screen's title */
    uint32_t ui_list_count;         /* items of the top screen's list */
    uint32_t battery_mv;
    uint32_t fs_count_ms;           /* background free count, +1; 0 = none yet */
    uint32_t ui_light;              /* 0 on, 1 dim, 2 dark (panel asleep) */
    uint32_t apps_loaded;           /* loadable apps in memory */
    uint32_t cpu_mhz;               /* the CPU clock now */
    uint32_t audio_icon;            /* the status bar: 0 speaker, 1 headphones, 2 headset, 3 muted */
} os_status_t;

extern volatile os_status_t os_status;

#endif
