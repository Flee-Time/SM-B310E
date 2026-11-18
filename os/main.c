/* B310E OS — start-up: hardware, kernel, card, self-test on the console,
 * then the user interface.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/arch.h"
#include "os/db.h"
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

static void measure_lcd(void *arg)
{
    *(uint32_t *)arg = lcd_measure();
}

/* The LCD's done flag, timed once for the log, after Home's first frame:
 * until 2026-11-18 it ran before ui_run and held Home back by up to ~45 ms
 * (a whole frame sent and slept out, then up to 14 ms polled with interrupts
 * off). At low priority this thread runs once the UI waits, its first frame
 * drawn; the measure itself runs in the UI thread (ui_call), the only one
 * that drives the LCD. */
static void boot_later(void *arg)
{
    uint32_t us = 0;
    bool ran = false;

    /* the UI loop's first pass (after Home is drawn and ui_run has set up
     * ui_call): the UI may sleep before that, in its settings or the card */
    for (int i = 0; i < 250 && !ui_alive_at; i++)
        thread_sleep(20);
    if (ui_alive_at)
        ran = ui_call(measure_lcd, &us, 5000);

    klog("lcd: transfer spacing %lu ms; done flag after %lu us%s",
         (unsigned long)lcd_transfer_ms(), (unsigned long)us,
         !ran ? " (not measured: the UI did not take it)" : us ? "" : " (none)");
}

static void main_thread(void *arg)
{
    /* the build on the screen while the card mounts: if that hangs, it says
     * which build it was */
    console_set_color(RGB(255, 210, 80), RGB(0, 0, 0));
    console_line(0, "B310E OS 0.3");
    console_set_color(RGB(255, 255, 255), RGB(0, 0, 0));
    console_printf("built %s\n", BUILD_DATE);
    console_flush();
    fs_init();
    db_init();                          /* the settings, before the UI reads them */
    {
        extern uint32_t boot_control;

        klog("boot: entered with control register %08lx (MMU %s, D-cache %s)",
             (unsigned long)boot_control, boot_control & 1 ? "on" : "off",
             boot_control & 4 ? "on" : "off");
        arch_fault_report();            /* a fault before this boot */
    }
    thread_create("bootlog", PRIO_LOW, 1536, boot_later, NULL);
    selftest_show_card();               /* the System screen reads fs_info itself */
    /* behind the UI, in developer mode (read from the store: the UI's settings
     * load later) or on a card new to the OS, as every QEMU test card is;
     * else from the shell only: it wrote 32 KB to the card at every boot */
    if (db_get_int("ui.dev_mode", 0) == 1 || db_new())
        selftest_start();
    ui_run(app_home());                 /* this thread becomes the UI */
}

void kmain(void)
{
    watchdog_stop();
    dma_quiesce();                      /* channels left by the firmware before
                                         * us wrote into our memory */
    dma_clock_idle();                   /* closed until audio or USB takes it */
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
