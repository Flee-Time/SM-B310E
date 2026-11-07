/* B310E OS — "USB storage": the SD card as a disk on a PC.
 *
 * Entering unmounts the card (fs_suspend), holds the CPU clock at 104 MHz
 * or more and starts USB mass storage; the screen shows the state, the
 * rate and the bytes moved. Back (RSoft), END or pulling the cable stops
 * USB and mounts the card again. Only one screen runs at a time and none
 * keeps files open across screens, so suspending on entry is enough.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/ui.h"
#include "os/usb.h"

#define RATE_WINDOW_MS  1000
#define ACTIVE_MS       1000            /* "Reading"/"Writing" this long after */

static struct {
    bool started, cable_seen, unplugged;
    uint32_t window_ms, window_bytes, rate_kib;
} st;

int usb_app_resume_result;              /* fs_resume() after the last session */

static void usb_enter(screen_t *s)
{
    memset(&st, 0, sizeof(st));
    fs_suspend();
    disk_set_readahead(0, 0);           /* the host may rewrite the FAT */
    cpufreq_hold();
    st.started = usb_start() == 0;
    st.cable_seen = usb_cable();
    st.window_ms = kernel_ticks();
}

/* The card log drops lines written while the card is away, so the
 * session is logged once it is back. */
static void usb_leave(screen_t *s)
{
    if (st.started)
        usb_stop();
    cpufreq_release();
    usb_app_resume_result = fs_resume();
    if (!st.started)
        klog("usb: not started (%s)", sd_present() ? "no memory" : "no card");
    else
        klog("usb: session %lu ended (%s%s): %lu KB in, %lu KB out, %lu cmds, "
             "%lu failed, %lu sd errors, %lu setups, %lu resets, %lu irqs%s",
             (unsigned long)usb_stat.sessions, st.unplugged ? "unplugged" : "back",
             usb_running() ? ", still running" : "",
             (unsigned long)(usb_stat.bytes_read / 1024),
             (unsigned long)(usb_stat.bytes_written / 1024),
             (unsigned long)usb_stat.commands, (unsigned long)usb_stat.failed,
             (unsigned long)usb_stat.sd_errors, (unsigned long)usb_stat.setups,
             (unsigned long)usb_stat.bus_resets, (unsigned long)usb_stat.irqs,
             usb_stat.polled ? ", polled" : "");
    if (st.started)
        usb_msc_trace_log();
    klog("usb: card %s", usb_app_resume_result == 0 ? "mounted again"
                                                    : "not mounted again");
}

static void usb_tick(screen_t *s)
{
    uint32_t now = kernel_ticks();
    uint32_t bytes = usb_stat.bytes_read + usb_stat.bytes_written;

    if (usb_cable()) {
        st.cable_seen = true;
    } else if (st.cable_seen) {
        st.unplugged = true;
        ui_pop();                       /* usb_leave() stops USB */
        return;
    }
    if (now - st.window_ms >= RATE_WINDOW_MS) {
        st.rate_kib = (uint32_t)((uint64_t)(bytes - st.window_bytes) * 1000u /
                                 (now - st.window_ms) / 1024u);
        st.window_ms = now;
        st.window_bytes = bytes;
    }
}

static void amount(char *buf, size_t n, const char *what, uint32_t bytes)
{
    if (bytes < 1024u * 1024u)
        snprintf(buf, n, "%s %lu KB", what, (unsigned long)(bytes / 1024));
    else
        snprintf(buf, n, "%s %lu.%lu MB", what, (unsigned long)(bytes >> 20),
                 (unsigned long)(((bytes & 0xfffffu) * 10u) >> 20));
}

static void usb_draw(screen_t *s)
{
    char buf[32];
    const char *state, *hint = "Back stops USB";
    uint16_t c = C_TEXT;
    int y = UI_TOP + 8;
    bool active = usb_stat.activity != USB_IDLE &&
                  kernel_ticks() - usb_stat.last_data_ms < ACTIVE_MS;

    if (!st.started) {
        state = sd_present() ? "No memory" : "No card";
        hint = "Back";
        c = C_BAD;
    } else if (!usb_cable() && !st.cable_seen) {
        state = "Plug in USB";
        hint = "a PC's USB port";
        c = C_WARN;
    } else if (usb_stat.ejected) {
        state = "Ejected";
        hint = "Safe to unplug";
        c = C_OK;
    } else {
        switch (usb_stat.state) {
        case USB_STARTING:    state = "Starting"; break;
        case USB_WAITING:     state = "Waiting for host"; c = C_DIM; break;
        case USB_ENUMERATING: state = "Connecting"; c = C_ACCENT; break;
        case USB_CONNECTED:
            c = C_OK;
            state = !active ? "Connected" :
                    usb_stat.activity == USB_READING ? "Reading" : "Writing";
            break;
        default:              state = "Stopped"; break;
        }
    }
    gfx_text_centered(y, state, c, FONT_LARGE, 1);
    y += 24;
    if (st.started) {
        snprintf(buf, sizeof(buf), "%lu KB/s", (unsigned long)(active ? st.rate_kib : 0));
        gfx_text_centered(y, buf, active ? C_TEXT : C_DIM, FONT_LARGE, 1);
        y += 24;
        amount(buf, sizeof(buf), "read", usb_stat.bytes_read);
        gfx_text(6, y, buf, C_TEXT, FONT_SMALL, 1, LCD_WIDTH - 12);
        y += 11;
        amount(buf, sizeof(buf), "written", usb_stat.bytes_written);
        gfx_text(6, y, buf, C_TEXT, FONT_SMALL, 1, LCD_WIDTH - 12);
        y += 11;
        snprintf(buf, sizeof(buf), "%lu commands, %lu failed",
                 (unsigned long)usb_stat.commands, (unsigned long)usb_stat.failed);
        gfx_text(6, y, buf, usb_stat.sd_errors ? C_BAD : C_DIM, FONT_SMALL, 1,
                 LCD_WIDTH - 12);
        y += 11;
        if (usb_stat.polled) {
            gfx_text(6, y, "irq storm: polling", C_WARN, FONT_SMALL, 1, LCD_WIDTH - 12);
            y += 11;
        }
    }
    gfx_text_centered(UI_BOTTOM - 12, hint, C_DIM, FONT_SMALL, 1);
}

static screen_t usb_screen = {
    .title = "USB storage", .right = "Back",
    .enter = usb_enter, .leave = usb_leave, .draw = usb_draw, .tick = usb_tick,
    .tick_ms = 500,
};

/* Test hook (os/tests/test-usb.py): QEMU points the home screen's tick
 * here, which opens this screen without going through the menu. */
void (*volatile usb_test_saved_tick)(screen_t *s);
void usb_test_hook(screen_t *s);

screen_t *app_usb(void)
{
    /* a reference, so the linker keeps the hook wherever app_usb is kept */
    __asm__ volatile("" : : "r"(usb_test_hook));
    return &usb_screen;
}

void usb_test_hook(screen_t *s)
{
    s->tick = usb_test_saved_tick;
    ui_push(app_usb());
}
