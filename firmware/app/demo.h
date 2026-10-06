/*
 * B310E-OS — app/demo.h
 *
 * Demo task entry points for the first bootable image (integration wave).
 *
 * Both tasks are COOPERATIVE: each MUST call task_yield() in its loop or it
 * starves the other task — and with only two tasks, the whole system. The
 * scheduler has no preemption (that is a later IRQ wave).
 *
 * Task entry signature matches kernel/sched.h: void fn(void *arg).
 * Created by arch/main.c after module_init_all(); they only start running
 * when sched_start() is called (so all drivers are guaranteed initialized).
 */

#ifndef B310E_OS_APP_DEMO_H
#define B310E_OS_APP_DEMO_H

#ifdef __cplusplus
extern "C" {
#endif

/* LCD banner + heartbeat task. Draws the boot banner, then blinks a pixel
 * and logs to the USB debug channel every N yields. Never returns. */
void demo_banner_task(void *arg);

/* Keypad echo task. Polls the matrix, drains keypad_queue, logs each key
 * to USB and draws its name on LCD row 3. Never returns. */
void demo_keypad_task(void *arg);

/* SD boot-path probe (Rockbox groundwork). Key-triggered (DIAL): runs the
 * full sdboot chain once — sdio_init -> sdcard_init -> fat_init ->
 * fat_find_path("ROCKBOX/ROCKBOX.TXT") -> fat_read_simple — logging every
 * stage, then exits. Proves the FAT32 layer on hardware inside os.bin. */
void demo_sd_task(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* B310E_OS_APP_DEMO_H */
