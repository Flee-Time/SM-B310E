/* B310E OS — the UI thread: screen stack, bars, keys, backlight, settings.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/app.h"
#include "os/arch.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/rtc.h"
#include "os/status.h"
#include "os/alarm.h"
#include "os/audio.h"
#include "os/media.h"
#include "os/notify.h"
#include "apps/wavio.h"
#include "os/ui.h"
#include "os/usb.h"

#define MAX_DEPTH      10
#define REPEAT_DELAY   450
#define REPEAT_EVERY   110
#define END_HOLD_MS    2000
#define TORCH_HOLD_MS  1000            /* hold 0: torch on/off (as in Rockbox) */
#define BUZZ_MS        80
#define TACTILE_MS     30                /* a key's feedback buzz */
#define MUSIC_MS       50                /* the music player's refill */
#define DIM_PERCENT    8
#define DARK_AFTER_MS  10000            /* after dimming */
#define SETTINGS_FILE  OS_DIR "/settings.txt"

ui_settings_t ui_settings = {
    60, 30, 0, 1, 0, 7 * 60,
    { 7, 6, 9, 8, 1 },               /* music, apps, alarm, calls; tactile light */
    1, 0,
    0, 1,                            /* UTC until set; the clock from the network */
};

static screen_t *stack[MAX_DEPTH];
static int depth;
static bool dirty;
static uint32_t last_input, end_down, next_repeat, next_second, next_tick, next_music;
static bool end_in_game;                /* END went down on a raw-key screen */
static bool collect;                    /* an app's screen left the stack */
static uint32_t zero_down;
static uint32_t center_down;            /* Center held on a panel host */
static screen_t *panel_hosts[4];
static int n_hosts;
static struct { screen_t *s; unsigned flags; } modes[4];   /* ui_screen_mode */

#define PANEL_HOLD_MS  600

/* Run a screen callback with its app as the current one (for ownership of
 * what it pushes and allocates). */
#define AS_OWNER(s, call) do {                     \
        app_t *saved_ = app_current;                 \
        app_current = (s)->app;                      \
        call;                                        \
        app_current = saved_;                        \
    } while (0)

static uint32_t tick_period(const screen_t *s)
{
    return s->tick_ms ? s->tick_ms : 1000;
}
static uint8_t held_key, swallow_key;
static int light;                       /* 0 on, 1 dim, 2 dark */
static int bat_mv = -1;
static int charging;                    /* CHARGE_* */

/* ---- settings ------------------------------------------------------------ */

/* The settings file: one "name=value" line each, every name in this table.
 * A name the file lacks keeps its default; one the table lacks is ignored,
 * so files written by older and newer builds both load. */
static const struct {
    const char *name;
    int *v;
    int min, max;
} setting_table[] = {
    { "brightness", &ui_settings.brightness, 5, 100 },
    { "screen_timeout", &ui_settings.screen_timeout_s, 0, 3600 },
    { "cpu_max", &ui_settings.cpu_max, 0, 1 },
    { "deep_idle", &ui_settings.deep_idle, 0, 1 },
    { "alarm_on", &ui_settings.alarm_on, 0, 1 },
    { "alarm_min", &ui_settings.alarm_min, 0, 24 * 60 - 1 },
    { "vol_music", &ui_settings.vol[VOL_MUSIC], 0, VOL_LEVELS },
    { "vol_apps", &ui_settings.vol[VOL_APP], 0, VOL_LEVELS },
    { "vol_alarm", &ui_settings.vol[VOL_ALARM], 0, VOL_LEVELS },
    { "vol_calls", &ui_settings.vol[VOL_CALL], 0, VOL_LEVELS },
    { "tactile", &ui_settings.vol[VOL_TACTILE], 0, 3 },
    { "keylight", &ui_settings.keylight, 0, 1 },
    { "usb_default", &ui_settings.usb_default, 0, 2 },
    { "time_zone", &ui_settings.tz_min, -720, 840 },
    { "net_time", &ui_settings.net_time, 0, 1 },
};

static void settings_load(void)
{
    char buf[512], *p;
    FIL f;
    UINT n = 0;

    if (f_open(&f, SETTINGS_FILE, FA_READ) != FR_OK)
        return;
    f_read(&f, buf, sizeof(buf) - 1, &n);
    f_close(&f);
    buf[n] = 0;
    for (p = buf; *p; ) {
        char *line = p, *eq;
        int v = 0;

        while (*p && *p != '\n')
            p++;
        if (*p)
            *p++ = 0;
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq++ = 0;
        while (*eq >= '0' && *eq <= '9')
            v = v * 10 + (*eq++ - '0');
        for (unsigned i = 0; i < ARRAY_SIZE(setting_table); i++)
            if (!strcmp(line, setting_table[i].name) && v >= setting_table[i].min &&
                v <= setting_table[i].max)
                *setting_table[i].v = v;
    }
}

void ui_settings_save(void)
{
    char buf[512];
    FIL f;
    UINT n;
    int len = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(setting_table); i++)
        len += snprintf(buf + len, sizeof(buf) - (size_t)len, "%s=%d\n",
                        setting_table[i].name, *setting_table[i].v);
    if (f_open(&f, SETTINGS_FILE, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        f_write(&f, buf, (UINT)len, &n);
        f_close(&f);
    }
}

void ui_apply_brightness(void)
{
    backlight_set(ui_settings.brightness);
}

/* ---- stack ---------------------------------------------------------------- */

#define RELOADING "One moment..."       /* an app between two loads */

static void publish(void)
{
    os_status.ui_depth = (uint32_t)depth;
    os_status.ui_title = app_reloading() ? RELOADING : depth ? stack[depth - 1]->title : NULL;
}

static unsigned top_mode(void);
static void raw_release_all(screen_t *s, uint32_t now);

void ui_push(screen_t *s)
{
    if (!s || depth == MAX_DEPTH)
        return;
    if (depth && top_mode() & UI_MODE_RAW_KEYS)
        raw_release_all(stack[depth - 1], kernel_ticks());  /* a game lets go of its keys */
    if (!s->app)
        s->app = app_self();
    stack[depth++] = s;
    if (s->enter)
        AS_OWNER(s, s->enter(s));
    next_tick = kernel_ticks() + tick_period(s);
    publish();
    dirty = true;
}

/* ---- screens in a mode of their own (games) ------------------------------ */
void ui_screen_mode(screen_t *s, unsigned flags)
{
    int free_slot = -1;

    for (int i = 0; i < (int)ARRAY_SIZE(modes); i++) {
        if (modes[i].s == s) {
            modes[i].flags = flags;
            if (!flags)
                modes[i].s = NULL;
            dirty = true;
            return;
        }
        if (!modes[i].s && free_slot < 0)
            free_slot = i;
    }
    if (flags && free_slot >= 0) {
        modes[free_slot].s = s;
        modes[free_slot].flags = flags;
        dirty = true;
    }
}

static unsigned top_mode(void)
{
    screen_t *s = depth ? stack[depth - 1] : NULL;

    for (int i = 0; s && i < (int)ARRAY_SIZE(modes); i++)
        if (modes[i].s == s)
            return modes[i].flags;
    return 0;
}

void ui_pop(void)
{
    screen_t *s;

    if (depth <= 1)
        return;
    s = stack[--depth];
    ui_screen_mode(s, 0);
    if (s->leave)
        AS_OWNER(s, s->leave(s));
    if (s->app) {
        collect = true;                 /* unloaded by the UI loop, never here:
                                         * this may run inside the app's code */
        if (s->free_on_pop)
            app_forget(s->app, s);
        else
            s->app = NULL;              /* tagged again when pushed again */
    }
    if (s->free_on_pop)
        kfree(s);
    next_tick = kernel_ticks() + tick_period(stack[depth - 1]);
    publish();
    dirty = true;
}

void ui_home(void)
{
    while (depth > 1)
        ui_pop();
}

void ui_redraw(void)
{
    dirty = true;
}

/* From any thread (a decoder, a network thread): draw again now, not at
 * the screen's next tick. The empty key event only wakes the UI loop; it
 * is not input (the screen stays dark, the timeout is not reset). */
void ui_collect_apps(void)
{
    collect = true;
}

/* Center held on s opens the quick panel; a short press reaches s when
 * Center is let go (so the hold can be told apart). */
void ui_panel_host(screen_t *s)
{
    if (n_hosts < (int)(sizeof(panel_hosts) / sizeof(panel_hosts[0])))
        panel_hosts[n_hosts++] = s;
}

static bool on_host(void)
{
    for (int i = 0; i < n_hosts; i++)
        if (stack[depth - 1] == panel_hosts[i])
            return true;
    return false;
}

void ui_refresh(void)
{
    dirty = true;
    keypad_inject(0, false);
}

int ui_depth(void)
{
    return depth;
}

/* ---- drawing --------------------------------------------------------------- */

static void draw_status(void)
{
    char buf[16];
    int pct, sh = gfx_text_height(FONT_SMALL, 1), y = (UI_STATUS_H - sh) / 2;
    int x = LCD_WIDTH - UI_PAD - 17;
    rtc_datetime_t t;

    gfx_fill(0, 0, LCD_WIDTH, UI_STATUS_H, C_BAR);
    rtc_get(&t);
    snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.min);
    gfx_text(UI_PAD + 2, y, buf, C_TEXT, FONT_SMALL, 1, 0);

    /* battery: a rounded outline, its level, the cap */
    pct = battery_percent(bat_mv);
    gfx_rect(x, 2, 15, 7, C_DIM);
    gfx_fill(x + 15, 4, 2, 3, C_DIM);
    if (pct > 0 || charging)
        gfx_fill(x + 2, 4, charging ? 11 : (pct * 11 + 99) / 100, 3,
                 charging ? C_OK : pct <= 15 ? C_BAD : C_TEXT);
    if (charging == CHARGE_DONE)
        snprintf(buf, sizeof(buf), "full");
    else if (charging)
        snprintf(buf, sizeof(buf), "chg");
    else if (pct >= 0)
        snprintf(buf, sizeof(buf), "%d%%", pct);
    else
        snprintf(buf, sizeof(buf), "--");
    x -= gfx_text_width(buf, FONT_SMALL, 1) + 3;
    gfx_text(x, y, buf, C_TEXT, FONT_SMALL, 1, 0);
    {                                   /* USB in the background */
        const char *u = usb_auto_label();

        if (u) {
            x -= gfx_text_width(u, FONT_SMALL, 1) + 6;
            gfx_text(x, y, u, C_ACCENT, FONT_SMALL, 1, 0);
        }
    }
    if (torch_get()) {                  /* a lit square */
        x -= 8;
        gfx_fill(x, (UI_STATUS_H - 4) / 2, 4, 4, C_WARN);
    }
    {                                   /* the notifications' icons after the time */
        int ix = UI_PAD + 2 + gfx_text_width("00:00", FONT_SMALL, 1) + 5;
        notify_t *n;

        for (int i = 0; (n = notify_get(i)) && ix + ICON_SIZE < x - 2; i++) {
            gfx_icon(ix, 0, n->icon ? n->icon : &icon_alarm, C_DIM);
            ix += ICON_SIZE + 1;
        }
    }
}

static void draw_bars(screen_t *s, const char *title)
{
    int bh = gfx_text_height(FONT_BODY, 1), y = UI_BOTTOM + (UI_SOFT_H - bh) / 2;

    gfx_fill(0, UI_STATUS_H, LCD_WIDTH, UI_TITLE_H, C_TITLE);
    gfx_text(UI_PAD + 2, UI_STATUS_H + (UI_TITLE_H - gfx_text_height(FONT_LARGE, 1)) / 2,
             title ? title : "", C_TEXT, FONT_LARGE, 1, LCD_WIDTH - 2 * UI_PAD - 4);

    gfx_fill(0, UI_BOTTOM, LCD_WIDTH, UI_SOFT_H, C_BAR);
    if (s->left)
        gfx_text(UI_PAD + 2, y, s->left, C_TEXT, FONT_BODY, 1, 44);
    if (s->center) {                    /* the main action, in a pill */
        int w = gfx_text_width(s->center, FONT_BODY, 1) + 14;

        gfx_round((LCD_WIDTH - w) / 2, UI_BOTTOM + 2, w, UI_SOFT_H - 4,
                  UI_RADIUS, C_ACCENT);
        gfx_text_centered(y, s->center, C_SEL_TEXT, FONT_BODY, 1);
    }
    if (s->right)
        gfx_text(LCD_WIDTH - UI_PAD - 2 - gfx_text_width(s->right, FONT_BODY, 1), y,
                 s->right, C_TEXT, FONT_BODY, 1, 44);
}

static void redraw(void)
{
    screen_t *s = stack[depth - 1], *under = depth > 1 ? stack[depth - 2] : NULL;
    bool overlay = s->overlay && under;

    if (light == 2)                     /* asleep: drawn again on wake */
        return;
    dirty = false;
    if (app_reloading()) {              /* an app between two loads: not the
                                         * screen under it for a moment */
        gfx_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
        gfx_text_centered(LCD_HEIGHT / 2 - 4, RELOADING, C_DIM, FONT_SMALL, 1);
        lcd_update();
        return;
    }
    if (top_mode() & UI_MODE_FULL) {    /* the whole LCD is the screen's */
        if (s->draw)
            AS_OWNER(s, s->draw(s));
        lcd_update();
        return;
    }
    gfx_fill(0, UI_TOP, LCD_WIDTH, UI_CONTENT_H, C_BG);
    if (overlay && under->draw)         /* a dialog over the screen under it */
        AS_OWNER(under, under->draw(under));
    if (s->draw)
        AS_OWNER(s, s->draw(s));
    draw_bars(s, overlay ? under->title : s->title);
    draw_status();
    lcd_update();
}

/* (API 2.9) From a screen's draw(). Only a full-screen one on top has the
 * panel to itself: under a dialog, or anything else, false, and it draws
 * as usual. */
bool ui_present(const uint16_t *px)
{
    if (!(top_mode() & UI_MODE_FULL))
        return false;
    lcd_present(px);
    return true;
}

static void splash(const char *text)
{
    lcd_wake();
    gfx_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    gfx_text_centered(LCD_HEIGHT / 2 - 8, text, C_TEXT, FONT_LARGE, 1);
    lcd_update();
}

void ui_power_off(void)
{
    splash("Powering off");
    klog("power off");
    rtc_save();
    fs_sync();
    thread_sleep(300);
    arch_power_off();
}

/* ---- calls from other threads ---------------------------------------------- */

static void (*volatile call_fn)(void *);
static void *volatile call_arg;
static app_t *volatile call_app;        /* the caller's: fn runs as that app */
static volatile bool call_taken;
static sem_t call_done;
static mutex_t call_lock;

bool ui_call(void (*fn)(void *arg), void *arg, uint32_t timeout_ms)
{
    uint32_t s;
    bool ok = true;

    mutex_lock(&call_lock, WAIT_FOREVER);
    s = arch_irq_save();
    call_arg = arg;
    call_app = app_self();
    call_taken = false;
    call_fn = fn;
    arch_irq_restore(s);
    keypad_inject(0, false);            /* wake the loop */
    if (sem_wait(&call_done, timeout_ms) != WAIT_OK) {
        s = arch_irq_save();
        if (!call_taken)
            call_fn = NULL;             /* withdrawn: it will not run */
        arch_irq_restore(s);
        if (call_taken)
            sem_wait(&call_done, WAIT_FOREVER);     /* it is running */
        else
            ok = false;
    }
    mutex_unlock(&call_lock);
    return ok;
}

static void serve_call(void)
{
    void (*fn)(void *);
    void *arg;
    app_t *app;
    uint32_t s = arch_irq_save();

    fn = call_fn;
    arg = call_arg;
    app = call_app;
    call_fn = NULL;
    if (fn)
        call_taken = true;
    arch_irq_restore(s);
    if (fn) {
        app_t *saved = app_current;

        app_current = app;
        fn(arg);
        app_current = saved;
        sem_post(&call_done);
    }
}

const char *ui_chain(const char *path, const char *what)
{
    FIL f;
    UINT n = 0;
    uint32_t size;
    uint8_t *image;

    if (f_open(&f, path, FA_READ) != FR_OK)
        return "The file is not on the card.";
    size = (uint32_t)f_size(&f);
    if (size < 1024 || size > 2u * 1024 * 1024) {
        f_close(&f);
        return "The file is not an image.";
    }
    image = kmalloc(size + 4);
    if (!image) {
        f_close(&f);
        return "Not enough memory.";
    }
    if (f_read(&f, image, size, &n) != FR_OK || n != size) {
        f_close(&f);
        kfree(image);
        return "Could not read it.";
    }
    f_close(&f);
    if (!arch_chain_fits(image, size)) {
        kfree(image);
        return "It does not fit.";
    }
    splash(what);
    klog("restart: %s (%lu bytes), no reset", path, (unsigned long)size);
    /* everything that runs on its own: the music, the radio, a recording,
     * USB, the vibrator; then the card's log */
    media_queue_clear();
    media_stop();
    wav_stop();
    fm_off();
    if (usb_running())
        usb_stop();
    vibrator_set(0);
    rtc_save();
    fs_sync();
    thread_sleep(200);
    arch_chain(image, size);
}

void ui_reboot(void)
{
    splash("Restarting");
    klog("reboot to the boot menu");
    rtc_save();
    fs_sync();
    thread_sleep(300);
    backlight_set(0);
    keylight_set(false);
    arch_reboot();
}

/* ---- input ------------------------------------------------------------------ */

static bool is_nav(uint8_t code)
{
    return code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT ||
           code == KEY_RIGHT;
}

static void dispatch(const key_event_t *ev)
{
    screen_t *s = stack[depth - 1];

    if (s->key)
        AS_OWNER(s, s->key(s, ev));
    else if (ev->code == KEY_RSOFT)
        ui_pop();
    dirty = true;
}

/* The screen timeout: dim, then dark (LED and keypad light off, panel
 * asleep). Until 2026-11-07 dark only set the LED to 0 and stopped the
 * refresh: on the phone the screen stayed lit and froze. */
static void set_light(int level)
{
    os_status.ui_light = (uint32_t)level;
    if (level == 2) {
        light = 2;
        backlight_set(0);
        keylight_set(false);
        lcd_sleep();
        return;
    }
    if (light == 2) {
        light = level;
        redraw();                       /* the frame the panel wakes to */
        lcd_wake();
        keylight_set(ui_settings.keylight);
    }
    light = level;
    if (level == 1)
        backlight_set(DIM_PERCENT);
    else
        ui_apply_brightness();
}

static void wake(void)
{
    if (light) {
        set_light(0);
        dirty = true;
    }
}

/* the vibrator at level (1..7) for ms; the UI loop ends it */
/* The vibrator's end is kept by a thread of its own above the UI: when the
 * UI loop ended it, a key whose handler took long (an app loading, half a
 * second) kept the 30 ms tactile buzz going all that time, as if stuck. */
static thread_t *buzz_thread;
static sem_t buzz_sem;
static mutex_t buzz_lock;
static uint32_t buzz_end;
static bool buzz_on;

static void buzz_main(void *arg)
{
    for (;;) {
        sem_wait(&buzz_sem, WAIT_FOREVER);
        for (;;) {
            int32_t left;

            mutex_lock(&buzz_lock, WAIT_FOREVER);
            left = (int32_t)(buzz_end - kernel_ticks());
            if (left <= 0 && buzz_on) {
                vibrator_set(0);
                buzz_on = false;
            }
            mutex_unlock(&buzz_lock);
            if (left <= 0)
                break;
            thread_sleep((uint32_t)left);
        }
    }
}

void ui_buzz(int level, uint32_t ms)
{
    if (level <= 0 || !ms)
        return;
    if (!buzz_thread) {
        sem_init(&buzz_sem, 0);
        mutex_init(&buzz_lock);
        buzz_thread = thread_create("buzz", PRIO_NORMAL + 3, 768, buzz_main, NULL);
        if (!buzz_thread)
            return;
    }
    mutex_lock(&buzz_lock, WAIT_FOREVER);
    vibrator_set(level > 7 ? 7 : level);
    buzz_on = true;
    buzz_end = kernel_ticks() + (ms > 5000 ? 5000 : ms);
    mutex_unlock(&buzz_lock);
    sem_post(&buzz_sem);
}

/* sound that goes on whatever screen is open */
static bool background_audio(void)
{
    return media_queue_active() || wav_playing() || wav_recording();
}

void ui_wake(void)
{
    last_input = kernel_ticks();
    wake();
}

/* The keys a raw-key screen (a game) has been given down: released for it
 * when the quick panel comes over it, so it does not walk on meanwhile. */
static uint32_t raw_held[4];
static screen_t *raw_screen;

static void raw_deliver(screen_t *s, const key_event_t *ev)
{
    uint32_t bit = 1u << (ev->code & 31), *w = &raw_held[(ev->code >> 5) & 3];

    if (s != raw_screen) {
        memset(raw_held, 0, sizeof(raw_held));
        raw_screen = s;
    }
    if (ev->down)
        *w |= bit;
    else
        *w &= ~bit;
    if (s->key)
        AS_OWNER(s, s->key(s, ev));
}

static void raw_release_all(screen_t *s, uint32_t now)
{
    for (unsigned code = 0; s == raw_screen && code < 128; code++)
        if (raw_held[code >> 5] & 1u << (code & 31)) {
            key_event_t up = { (uint8_t)code, 0, now };

            raw_deliver(s, &up);
        }
}

/* * and # together: the quick panel (volume, the torch ...) over any
 * screen, games too; again: it closes. The key pressed first went to the
 * screen, which gets its release; the chord's other presses and releases
 * go nowhere. */
static uint8_t chord_down, chord_eat;   /* bit 0: *, bit 1: # */

static bool chord(const key_event_t *ev, uint32_t now)
{
    uint8_t bit;
    screen_t *s = depth ? stack[depth - 1] : NULL;

    if (ev->code != KEY_STAR && ev->code != KEY_HASH)
        return false;
    bit = ev->code == KEY_STAR ? 1 : 2;
    if (!ev->down) {
        chord_down &= (uint8_t)~bit;
        if (chord_eat & bit) {
            chord_eat &= (uint8_t)~bit;
            return true;
        }
        return false;
    }
    chord_down |= bit;
    if (chord_down != 3 || !s)
        return false;
    chord_eat = 3;
    if (light)
        wake();
    held_key = swallow_key = 0;
    center_down = zero_down = 0;
    if (top_mode() & UI_MODE_RAW_KEYS)
        raw_release_all(s, now);        /* the first of the two, and the rest */
    if (s == ui_panel()) {
        ui_pop();
    } else {
        ui_buzz(4, 40);
        ui_push(ui_panel());
    }
    klog("ui: quick panel by * + #");
    return true;
}

static void on_key(const key_event_t *ev, uint32_t now)
{
    os_status.keys++;
    os_status.last_key = ev->code | (uint32_t)ev->down << 8;
    last_input = now;

    if (app_reloading()) {              /* the keys are not for the screen under it */
        if (ev->code == KEY_END && !ev->down) {
            end_down = 0;
            end_in_game = false;
        }
        held_key = swallow_key = 0;
        center_down = zero_down = 0;
        return;
    }
    if (chord(ev, now))
        return;
    if (top_mode() & UI_MODE_RAW_KEYS) {  /* a game: every press and release */
        screen_t *s = stack[depth - 1];

        if (ev->down && light)
            wake();
        if (ev->code == KEY_END)
            end_down = ev->down ? (now ? now : 1) : 0;  /* held: power off */
        if (ev->code == KEY_END)
            end_in_game = ev->down;     /* its release is the game's too */
        held_key = swallow_key = 0;
        center_down = zero_down = 0;
        raw_deliver(s, ev);
        return;
    }

    if (ev->down && light) {            /* the first key only wakes */
        wake();
        swallow_key = ev->code;
        return;
    }
    if (!ev->down && ev->code == swallow_key) {
        swallow_key = 0;
        return;
    }
    if (ev->down && ui_settings.vol[VOL_TACTILE]) {
        /* tactile feedback: a short buzz a press, at the chosen strength */
        static const uint8_t strength[4] = { 0, 2, 4, 7 };

        ui_buzz(strength[ui_settings.vol[VOL_TACTILE] & 3], TACTILE_MS);
    }
    if (ev->code == KEY_0)
        zero_down = ev->down ? now : 0;
    if (ev->code == KEY_CENTER && (center_down || on_host())) {
        if (ev->down) {
            center_down = now ? now : 1;
        } else if (center_down) {       /* let go before the hold: a press */
            key_event_t press = { KEY_CENTER, 1, now };

            center_down = 0;
            dispatch(&press);
        }
        return;
    }
    if (ev->code == KEY_END) {
        if (ev->down) {
            end_down = now;
            end_in_game = false;
        } else if (end_down) {
            end_down = 0;
            if (end_in_game) {          /* pressed in a game that has ended: */
                end_in_game = false;    /* not a press of the screen's */
                return;
            }
            ui_home();
        }
        return;
    }
    if (ev->down) {
        if (is_nav(ev->code)) {
            held_key = ev->code;
            next_repeat = now + REPEAT_DELAY;
        }
        dispatch(ev);
    } else if (ev->code == held_key) {
        held_key = 0;
    }
}

/* Battery and power state to the card log, to read a day's drain and the
 * clock against the RTC (uptime and RTC time side by side). */
static void drain_log(uint32_t now)
{
    rtc_datetime_t t;
    uint32_t load = 1000 - kernel_idle_permille();

    rtc_get(&t);
    klog("battery: %d mV %d%%%s, load %lu.%lu%%, %d MHz, screen %s, rtc %02d:%02d:%02d, up %lu s",
         bat_mv, battery_percent(bat_mv),
         charging == CHARGE_CHARGING ? ", charging" : charging == CHARGE_DONE ? ", charged" : "",
         (unsigned long)(load / 10), (unsigned long)(load % 10), clock_mhz(),
         light == 0 ? "on" : light == 1 ? "dim" : "dark", t.hour, t.min, t.sec,
         (unsigned long)(now / 1000));
}

static void every_second(uint32_t now)
{
    static unsigned n;
    uint32_t timeout = (uint32_t)ui_settings.screen_timeout_s * 1000u;
    int h, m, sec;

    rtc_time(&h, &m, &sec);
    alarm_check(h, m);
    usb_auto_tick();
    notify_tick();
    if (n % 30 == 0) {                  /* the ADC is slow: twice a minute */
        charging = charge_state();
        bat_mv = battery_mv();
        os_status.battery_mv = (uint32_t)bat_mv;
    }
    if (n && n % 600 == 0)              /* the drain log: every 10 minutes
                                         * (not at boot: its load is boot work) */
        drain_log(now);
    if (n && n % 600 == 0)
        rtc_save();
    n++;
    os_status.ticks = now;
    os_status.idle_permille = kernel_idle_permille();
    os_status.cpu_mhz = (uint32_t)clock_mhz();
    os_status.heap_free = heap_free_bytes();
    os_status.stray_irqs = irq_stray_count;

    if (top_mode() & UI_MODE_AWAKE)
        last_input = now;               /* a game: no timeout */
    if (timeout && light == 0 && now - last_input > timeout)
        set_light(1);
    else if (timeout && light == 1 && now - last_input > timeout + DARK_AFTER_MS)
        set_light(2);
    dirty = true;                       /* the clock */
}

/* The next tick after now, on the clock's phase: a screen ticking every
 * 500 ms then redraws together with the clock's second, not 500 ms apart
 * from it (two draws a second instead of three; the load does not see-saw). */
static uint32_t tick_after(uint32_t now, uint32_t period)
{
    int32_t d = (int32_t)(next_second - now);
    uint32_t t;

    if (d <= 0 || !period)
        return now + period;
    t = next_second - (uint32_t)d / period * period;
    if ((int32_t)(t - now) <= 0)
        t += period;
    return t;
}

/* An app's thread ended (kernel, interrupts off): the UI loop looks
 * whether the app can go (its screens may have left the stack before). */
static void app_thread_ended(void)
{
    collect = true;
    keypad_inject(0, false);
}

void ui_run(screen_t *home)
{
    key_event_t ev;

    thread_owned_exit = app_thread_ended;

    settings_load();
    rtc_restore();
    cpufreq_set_mode(ui_settings.cpu_max ? CPUFREQ_MAX : CPUFREQ_AUTO);
    kernel_set_tickless(ui_settings.deep_idle);
    ui_apply_brightness();
    keylight_set(ui_settings.keylight);
    last_input = kernel_ticks();
    next_second = last_input;
    sem_init(&call_done, 0);
    mutex_init(&call_lock);
    ui_push(home);
    redraw();
    klog("boot: home drawn at %lu ms", (unsigned long)kernel_ticks());

    for (;;) {
        uint32_t now = kernel_ticks(), wait = 1000;

        if ((int32_t)(next_second - now) < (int32_t)wait)
            wait = (int32_t)(next_second - now) > 0 ? next_second - now : 0;
        if (held_key && (int32_t)(next_repeat - now) < (int32_t)wait)
            wait = (int32_t)(next_repeat - now) > 0 ? next_repeat - now : 0;
        if ((int32_t)(next_tick - now) < (int32_t)wait)
            wait = (int32_t)(next_tick - now) > 0 ? next_tick - now : 0;
        if ((end_down || zero_down) && wait > 100)
            wait = 100;
        if (center_down && wait > 50)
            wait = 50;
        if (background_audio() && wait > MUSIC_MS)
            wait = MUSIC_MS;
        if (dirty && light < 2) {     /* dark: drawn on wake, do not spin */
            uint32_t busy = lcd_busy_ms();  /* the keys in the meantime, and then
                                             * what is newest, not what was */
            if (busy < wait)
                wait = busy;
        }

        if (keypad_get(&ev, wait) == WAIT_OK && ev.code)
            on_key(&ev, kernel_ticks());
        if (call_fn)
            serve_call();
        now = kernel_ticks();
        if (background_audio() && (int32_t)(now - next_music) >= 0) {
            next_music = now + MUSIC_MS;    /* on any screen: the play queue, */
            media_queue_tick();             /* and WAV files playing or recording */
            wav_pump();
        }

        if (held_key && (int32_t)(now - next_repeat) >= 0) {
            key_event_t rep = { held_key, 1, now };
            next_repeat = now + REPEAT_EVERY;
            dispatch(&rep);
        }
        if (end_down && now - end_down >= END_HOLD_MS)
            ui_power_off();
        if (center_down && now - center_down >= PANEL_HOLD_MS) {
            center_down = 0;            /* held: the quick panel, not a press */
            ui_buzz(4, 40);
            ui_push(ui_panel());
        }
        if (zero_down && now - zero_down >= TORCH_HOLD_MS) {
            zero_down = 0;              /* one toggle per press */
            torch_set(!torch_get());
            klog("torch %s", torch_get() ? "on" : "off");
            ui_buzz(4, BUZZ_MS);
            dirty = true;
        }
        if ((int32_t)(now - next_second) >= 0) {
            next_second = now + 1000;
            every_second(now);
        }
        if ((int32_t)(now - next_tick) >= 0) {
            screen_t *s = stack[depth - 1];

            next_tick = tick_after(now, tick_period(s));
            if (s->tick) {
                AS_OWNER(s, s->tick(s));
                dirty = true;
            }
        }
        if (collect) {
            bool reloading = app_reloading();

            collect = false;
            app_collect(stack, depth);
            if (reloading && !app_reloading()) {
                publish();
                dirty = true;
            }
        }
        if (dirty && light < 2 && !lcd_busy_ms())
            redraw();
    }
}
