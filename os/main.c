/* B310E OS — start-up: hardware, kernel, card, self-test on the console,
 * then the user interface.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/dsp.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/status.h"
#include "os/ui.h"
#include "build_info.h"

extern char __heap_start[], __heap_end[];

volatile os_status_t os_status __attribute__((used));

void selftest_start(void);
void selftest_show_card(void);

static void main_thread(void *arg)
{
    console_set_color(RGB(255, 210, 80), RGB(0, 0, 0));
    console_line(0, "B310E OS 0.3");
    console_set_color(RGB(255, 255, 255), RGB(0, 0, 0));
    console_printf("built %s\n", BUILD_DATE);
    console_flush();
    fs_init();
    {
        uint32_t us = lcd_measure();

        extern uint32_t boot_control;

        klog("boot: entered with control register %08lx (MMU %s, D-cache %s)",
             (unsigned long)boot_control, boot_control & 1 ? "on" : "off",
             boot_control & 4 ? "on" : "off");
        klog("lcd: transfer spacing %lu ms; done flag after %lu us%s",
             (unsigned long)lcd_transfer_ms(), (unsigned long)us,
             us ? "" : " (none)");
    }
    selftest_show_card();
    selftest_start();                   /* behind the UI */
    ui_run(app_home());                 /* this thread becomes the UI */
}

void kmain(void)
{
    watchdog_stop();
    dma_quiesce();                      /* channels left by the firmware before
                                         * us wrote into our memory */
    clock_init();
    dsp_hold_reset();                   /* unused: its boot ROM need not run */
    torch_set(false);                   /* the pin set up once, as Rockbox does
                                         * in power_init: the first toggle was
                                         * lost on the phone otherwise */
    heap_init(__heap_start, __heap_end);
    os_status.magic = OS_MAGIC;

    lcd_init();
    console_init();
    console_reserve(1);
    backlight_set(60);
    console_line(0, "B310E OS 0.3");
    lcd_update();

    timer_init(1000, kernel_tick);
    keypad_init();
    sleep_chip_init();                  /* the chip's power set-up, as stock's boot */
    thread_create("ui", PRIO_NORMAL, 6144, main_thread, NULL);
    kernel_start();
}
