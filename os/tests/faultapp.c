/* A test app for os/tests/test-fault.py (on no card of its own): its thread
 * stores into the NOR window, which the MMU keeps read-only. The OS sends the
 * thread to the handler it set (app_on_fault); the handler's own store then
 * ends the thread, and the OS goes on. The test reads fault_stage. LSoft
 * runs an undefined instruction in the UI's thread: the fault screen, a restart,
 * and the next boot's log names it.
 *
 *   make PYTHON=python ../build/os/tests/faultapp.app      (from os/)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"

volatile uint32_t fault_stage;          /* 1 storing, 2 in the handler; 98, 99: wrong */
static volatile uintptr_t first = 0x100, second = 0x200;   /* (not constants: no warning) */

static void handler(const char *why)
{
    klog("faulttest: handler: %s", why);
    fault_stage = 2;
    *(volatile uint32_t *)second = 2;   /* again: this time the thread ends */
    fault_stage = 99;
    for (;;)
        thread_sleep(1000);
}

static void worker(void *arg)
{
    (void)arg;
    app_on_fault(handler);
    fault_stage = 1;
    *(volatile uint32_t *)first = 1;
    fault_stage = 98;
}

static void draw(screen_t *s)
{
    (void)s;
    gfx_text_centered(UI_TOP + 30, "Fault test", C_TEXT, FONT_SMALL, 1);
}

static void key(screen_t *s, const key_event_t *ev)
{
    (void)s;
    if (ev->code == KEY_RSOFT)
        ui_pop();
    else if (ev->code == KEY_LSOFT)     /* the UI's thread, not the app's: the
                                         * fault screen, a restart, the record */
        __asm__ volatile(".short 0xde00");      /* undefined in Thumb */
}

static screen_t screen = {
    .title = "Fault test", .left = "Fault", .right = "Close", .draw = draw, .key = key,
};

static screen_t *start(void)
{
    fault_stage = 0;
    thread_create("faulttest", PRIO_LOW, 2048, worker, NULL);
    return &screen;
}

APP("FaultTest", start, NULL, NULL);
