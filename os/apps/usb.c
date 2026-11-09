/* B310E OS — USB: "Storage" or "Network", one at a time.
 *
 * "USB storage": the SD card as a disk on a PC. Entering unmounts the
 * card (fs_suspend), holds the CPU clock at 104 MHz or more and starts
 * USB mass storage; the screen shows the state, the rate and the bytes
 * moved. Back (RSoft), END or pulling the cable stops USB and mounts the
 * card again. Only one screen runs at a time and none keeps files open
 * across screens, so suspending on entry is enough.
 *
 * "USB network": the phone as a USB network adapter (RNDIS) with the
 * development shell (net/); the card stays mounted, the shell uses it.
 * The same clock hold, Back, END and unplug. Apps the shell starts are
 * pushed above this screen; leaving it closes them.
 *
 * In the background (Settings -> USB, "Mode"): plugging a data
 * cable in starts the chosen function with no screen at all; pulling it
 * out stops it (and mounts the card again after storage). A plain charger
 * (no PC answers within NO_HOST_MS, NO_HOST_TRIES times) stops it again, so
 * the CPU clock is not held for nothing. Each try is a new connection: a PC
 * still busy with the last one (the phone reset just after it came up, or
 * the boot ROM's own at a power-on) does not see the first, and without
 * another nothing would bring it back but a replug (seen on the phone,
 * 2026-11-09: 0 resets, 0 setups). Only the plug starts it: leaving a USB screen
 * with the cable in does not. The status bar shows USB (disk) or NET.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/app.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/media.h"
#include "os/net.h"
#include "os/notify.h"
#include "os/ui.h"
#include "os/usb.h"

#define RATE_WINDOW_MS  1000
#define ACTIVE_MS       1000            /* "Reading"/"Writing" this long after */

static struct {
    bool started, cable_seen, unplugged;
    uint32_t window_ms, window_bytes, rate_kib;
    uint32_t began;                     /* kernel_ticks() at the start */
} st;

int usb_app_resume_result;              /* fs_resume() after the last session */

static void auto_stop(bool unplugged);

static void storage_begin(void)
{
    memset(&st, 0, sizeof(st));
    media_queue_clear();                /* the music reads the card */
    media_stop();
    fs_suspend();
    disk_set_readahead(0, 0);           /* the host may rewrite the FAT */
    cpufreq_hold();
    /* 208 MHz for the session: every 64-byte packet is an interrupt and a
     * FIFO refill from PSRAM, and the governor kept 104 MHz (a Windows
     * mount reads some 7 MB at about 350 KB/s, 2026-11-07) */
    cpufreq_set_mode(CPUFREQ_MAX);
    st.began = kernel_ticks();
    st.started = usb_start() == 0;
    st.cable_seen = usb_cable();
    st.window_ms = kernel_ticks();
}

/* The card log drops lines written while the card is away, so the
 * session is logged once it is back. */
static void storage_end(void)
{
    if (st.started)
        usb_stop();
    cpufreq_set_mode(ui_settings.cpu_max ? CPUFREQ_MAX : CPUFREQ_AUTO);
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
    if (st.started) {
        uint32_t s = (kernel_ticks() - st.began) / 1000;

        klog("usb: %lu s; %lu KB/s out of the card, %lu KB/s in, at 208 MHz",
             (unsigned long)s, (unsigned long)(s ? usb_stat.bytes_read / 1024 / s : 0),
             (unsigned long)(s ? usb_stat.bytes_written / 1024 / s : 0));
        usb_msc_trace_log();
    }
    klog("usb: card %s", usb_app_resume_result == 0 ? "mounted again"
                                                    : "not mounted again");
}

static void usb_enter(screen_t *s)
{
    auto_stop(false);                   /* the screen takes over */
    storage_begin();
}

static void usb_leave(screen_t *s)
{
    storage_end();
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

/* ---- network ------------------------------------------------------------- */

static struct {
    bool started, cable_seen, unplugged;
    int depth;                          /* this screen's place on the stack */
} nt;

static void net_begin(void)
{
    memset(&nt, 0, sizeof(nt));
    cpufreq_hold();
    nt.started = usb_start_function(USB_FN_NETWORK) == 0;
    nt.cable_seen = usb_cable();
}

static void net_end(void)
{
    if (nt.started)
        usb_stop();
    cpufreq_release();
    nt.depth = 0;
    if (!nt.started)
        klog("usb: network not started (no memory)");
    else
        klog("usb: network session %lu ended (%s%s): %lu setups, %lu resets, %lu irqs, "
             "%lu RNDIS messages, %lu frames in, %lu out, %lu shell sessions%s",
             (unsigned long)usb_stat.sessions, nt.unplugged ? "unplugged" : "back",
             usb_running() ? ", still running" : "", (unsigned long)usb_stat.setups,
             (unsigned long)usb_stat.bus_resets, (unsigned long)usb_stat.irqs,
             (unsigned long)net_stat.rndis_msgs, (unsigned long)net_stat.rx_frames,
             (unsigned long)net_stat.tx_frames, (unsigned long)net_stat.shell_sessions,
             usb_stat.polled ? ", polled" : "");
}

static void net_enter(screen_t *s)
{
    auto_stop(false);                   /* the screen takes over */
    net_begin();
    nt.depth = ui_depth();
}

static void net_leave(screen_t *s)
{
    net_end();
}

/* the shell's "run" and "close" (in the UI thread): back to this screen,
 * or home when the network runs in the background (no screen) */
void usb_net_close_apps(void)
{
    if (!nt.depth) {
        ui_home();
        return;
    }
    while (ui_depth() > nt.depth)
        ui_pop();
}

static void net_tick(screen_t *s)
{
    if (usb_cable()) {
        nt.cable_seen = true;
    } else if (nt.cable_seen) {
        nt.unplugged = true;
        ui_pop();                       /* net_leave() stops USB */
    }
}

static void net_draw(screen_t *s)
{
    char buf[40], ip[20];
    const char *state;
    uint16_t c = C_TEXT;
    int y = UI_TOP + 8;

    if (!nt.started) {
        state = "No memory";
        c = C_BAD;
    } else if (!usb_cable() && !nt.cable_seen) {
        state = "Plug in USB";
        c = C_WARN;
    } else {
        switch (usb_stat.state) {
        case USB_STARTING:    state = "Starting"; break;
        case USB_WAITING:     state = "Waiting for host"; c = C_DIM; break;
        case USB_ENUMERATING: state = "Connecting"; c = C_ACCENT; break;
        case USB_CONNECTED:
            state = net_stat.link ? "Online" : "Connected";
            c = net_stat.link ? C_OK : C_ACCENT;
            break;
        default:              state = "Stopped"; break;
        }
    }
    gfx_text_centered(y, state, c, FONT_LARGE, 1);
    y += 22;
    /* the address the PC reaches us at: 192.168.7.1 after our DHCP lease,
     * 169.254.7.1 when the PC gave itself a link-local one */
    snprintf(ip, sizeof(ip), "%lu.%lu.%lu.%lu", (unsigned long)(net_self >> 24),
             (unsigned long)(net_self >> 16 & 255), (unsigned long)(net_self >> 8 & 255),
             (unsigned long)(net_self & 255));
    gfx_text_centered(y, ip, C_TEXT, FONT_LARGE, 1);
    y += 18;
    if (nt.started) {
        snprintf(buf, sizeof(buf), "PC %s", net_stat.dhcp_acks ? "192.168.7.2 (DHCP)" :
                 net_self == NET_LL_IP ? "link-local 169.254.x.x" : "no address yet");
        gfx_text(6, y, buf, net_stat.dhcp_acks || net_self == NET_LL_IP ? C_TEXT : C_DIM,
                 FONT_SMALL, 1, LCD_WIDTH - 12);
        y += 11;
        snprintf(buf, sizeof(buf), "%lu frames in, %lu out",
                 (unsigned long)net_stat.rx_frames, (unsigned long)net_stat.tx_frames);
        gfx_text(6, y, buf, C_DIM, FONT_SMALL, 1, LCD_WIDTH - 12);
        y += 11;
        snprintf(buf, sizeof(buf), "shell: %lu open, %lu cmds",
                 (unsigned long)net_stat.shell_open, (unsigned long)net_stat.shell_commands);
        gfx_text(6, y, buf, C_DIM, FONT_SMALL, 1, LCD_WIDTH - 12);
        y += 11;
        if (usb_stat.polled)
            gfx_text(6, y, "irq storm: polling", C_WARN, FONT_SMALL, 1, LCD_WIDTH - 12);
    }
    snprintf(buf, sizeof(buf), "telnet %s", ip);
    gfx_text_centered(UI_BOTTOM - 12, buf, C_DIM, FONT_SMALL, 1);
}

static screen_t net_screen = {
    .title = "USB network", .right = "Back",
    .enter = net_enter, .leave = net_leave, .draw = net_draw, .tick = net_tick,
    .tick_ms = 500,
};

/* ---- the choice ------------------------------------------------------------ */

static list_t menu_list;

static void menu_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    snprintf(text, (size_t)tn, "%s", i == 0 ? "Storage" : "Network");
    snprintf(detail, (size_t)dn, "%s", i == 0 ? "disk" : "shell");
}

static const icon_t *menu_icon(void *ctx, int i)
{
    return &icon_usb;
}

static void menu_key(screen_t *s, const key_event_t *ev)
{
    if (list_key(&menu_list, ev))
        return;
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT)
        ui_push(menu_list.sel == 0 ? app_usb() : app_usb_net());
    else if (ev->code == KEY_RSOFT)
        ui_pop();
}

static void menu_draw(screen_t *s)
{
    list_draw(&menu_list);
}

static screen_t menu_screen = {
    .title = "USB", .left = "Select", .center = "OK", .right = "Back",
    .draw = menu_draw, .key = menu_key,
};

/* ---- in the background: the function chosen in Settings, on the plug ---- */

#define NO_HOST_MS 5000                 /* a try: nobody enumerates us */
#define NO_HOST_TRIES 4                 /* then a charger */

static int auto_tries;                  /* connections made since the plug */
static bool auto_retry;                 /* NO_HOST_MS passed: connect again */
static int auto_fn = -1;                /* USB_FN_* running in the background, -1 none */
static bool cable_was;
static uint32_t auto_since;

static notify_t note;

static void note_act(notify_t *n, int a)
{
    auto_stop(false);
}

static void note_post(void)
{
    note.icon = &icon_usb;
    if (auto_fn == USB_FN_STORAGE) {
        snprintf(note.title, sizeof(note.title), "USB storage");
        snprintf(note.text, sizeof(note.text), "The card is the PC's");
    } else {
        snprintf(note.title, sizeof(note.title), "USB network");
        snprintf(note.text, sizeof(note.text), "Shell at %lu.%lu.%lu.%lu",
                 (unsigned long)(net_self >> 24), (unsigned long)(net_self >> 16 & 255),
                 (unsigned long)(net_self >> 8 & 255), (unsigned long)(net_self & 255));
    }
    note.action[0] = "Stop";
    note.act = note_act;
    notify_post(&note);
}

static void auto_stop(bool unplugged)
{
    if (auto_fn < 0)
        return;
    notify_remove(&note);
    klog("usb: background %s stopped (%s)", auto_fn == USB_FN_STORAGE ? "storage" : "network",
         unplugged ? "unplugged" : "no PC, or a USB screen or Stop");
    if (auto_fn == USB_FN_STORAGE) {
        st.unplugged = unplugged;
        storage_end();
    } else {
        nt.unplugged = unplugged;
        net_end();
    }
    auto_fn = -1;
}

/* the UI, every second */
void usb_auto_tick(void)
{
    bool cable = usb_cable();
    bool plugged = cable && !cable_was;

    cable_was = cable;
    if (auto_fn >= 0) {
        if (!cable) {
            auto_stop(true);
        } else if (usb_stat.state < USB_ENUMERATING && kernel_ticks() - auto_since > NO_HOST_MS) {
            auto_retry = auto_tries < NO_HOST_TRIES;
            if (!auto_retry)
                klog("usb: no PC on the cable: a charger");
            auto_stop(false);
        }
        return;
    }
    if (!plugged && !(auto_retry && cable)) {
        auto_retry = false;
        return;
    }
    auto_retry = false;
    if (plugged)
        auto_tries = 0;
    if (!ui_settings.usb_default || usb_running())
        return;
    auto_tries++;
    auto_since = kernel_ticks();
    if (ui_settings.usb_default == 1) {
        storage_begin();
        if (!st.started) {
            storage_end();
            return;
        }
        auto_fn = USB_FN_STORAGE;
    } else {
        net_begin();
        if (!nt.started) {
            net_end();
            return;
        }
        auto_fn = USB_FN_NETWORK;
    }
    klog("usb: background %s on the plug (try %d)",
         auto_fn == USB_FN_STORAGE ? "storage" : "network", auto_tries);
    if (auto_tries == 1)
        note_post();
}

/* "USB" or "NET" in the status bar while one runs, else NULL */
const char *usb_auto_label(void)
{
    if (auto_fn == USB_FN_STORAGE)
        return "USB";
    if (auto_fn == USB_FN_NETWORK)
        return net_stat.link ? "NET" : "net";
    return NULL;
}

/* Test hooks (os/tests/test-usb.py, test-net.py): QEMU points the home
 * screen's tick here, which opens the screen without going through the
 * menu. */
void (*volatile usb_test_saved_tick)(screen_t *s);
void usb_test_hook(screen_t *s);
void usb_test_hook_net(screen_t *s);

screen_t *app_usb(void)
{
    /* a reference, so the linker keeps the hook wherever app_usb is kept */
    __asm__ volatile("" : : "r"(usb_test_hook));
    return &usb_screen;
}

screen_t *app_usb_net(void)
{
    __asm__ volatile("" : : "r"(usb_test_hook_net));
    return &net_screen;
}

screen_t *app_usb_menu(void)
{
    /* opened anew: from the top (back from a screen it opened, it stays) */
    menu_list.sel = menu_list.top = 0;
    menu_list.count = 2;
    menu_list.item = menu_item;
    menu_list.icon = menu_icon;
    return &menu_screen;
}

void usb_test_hook(screen_t *s)
{
    s->tick = usb_test_saved_tick;
    ui_push(app_usb());
}

void usb_test_hook_net(screen_t *s)
{
    s->tick = usb_test_saved_tick;
    ui_push(app_usb_net());
}
