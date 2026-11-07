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
#include "os/ui.h"

#define MAX_DEPTH      10
#define REPEAT_DELAY   450
#define REPEAT_EVERY   110
#define END_HOLD_MS    2000
#define TORCH_HOLD_MS  1000            /* hold 0: torch on/off (as in Rockbox) */
#define BUZZ_MS        80
#define DIM_PERCENT    8
#define DARK_AFTER_MS  10000            /* after dimming */
#define SETTINGS_FILE  OS_DIR "/settings.txt"

ui_settings_t ui_settings = { 60, 30, 0 };

static screen_t *stack[MAX_DEPTH];
static int depth;
static bool dirty;
static uint32_t last_input, end_down, next_repeat, next_second, next_tick;
static bool collect;                    /* an app's screen left the stack */
static uint32_t zero_down, buzz_until;

/* Run a screen callback with its app as the current one (for ownership of
 * what it pushes and allocates). */
#define AS_OWNER(s, call) do {                                                      app_t *saved_ = app_current;                                                app_current = (s)->app;                                                     call;                                                                       app_current = saved_;                                                   } while (0)

static uint32_t tick_period(const screen_t *s)
{
    return s->tick_ms ? s->tick_ms : 1000;
}
static uint8_t held_key, swallow_key;
static int light;                       /* 0 on, 1 dim, 2 dark */
static int bat_mv = -1;
static bool charging;

/* ---- settings ------------------------------------------------------------ */

static void settings_load(void)
{
    char buf[256], *p;
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
        if (!strcmp(line, "brightness") && v >= 5 && v <= 100)
            ui_settings.brightness = v;
        else if (!strcmp(line, "screen_timeout"))
            ui_settings.screen_timeout_s = v;
        else if (!strcmp(line, "cpu_max"))
            ui_settings.cpu_max = v != 0;
    }
}

void ui_settings_save(void)
{
    char buf[96];
    FIL f;
    UINT n;
    int len = snprintf(buf, sizeof(buf), "brightness=%d\nscreen_timeout=%d\ncpu_max=%d\n",
                       ui_settings.brightness, ui_settings.screen_timeout_s,
                       ui_settings.cpu_max);

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

static void publish(void)
{
    os_status.ui_depth = (uint32_t)depth;
    os_status.ui_title = depth ? stack[depth - 1]->title : NULL;
}

void ui_push(screen_t *s)
{
    if (!s || depth == MAX_DEPTH)
        return;
    if (!s->app)
        s->app = app_current;
    stack[depth++] = s;
    if (s->enter)
        AS_OWNER(s, s->enter(s));
    next_tick = kernel_ticks() + tick_period(s);
    publish();
    dirty = true;
}

void ui_pop(void)
{
    screen_t *s;

    if (depth <= 1)
        return;
    s = stack[--depth];
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
    if (charging)
        snprintf(buf, sizeof(buf), "chg");
    else if (pct >= 0)
        snprintf(buf, sizeof(buf), "%d%%", pct);
    else
        snprintf(buf, sizeof(buf), "--");
    x -= gfx_text_width(buf, FONT_SMALL, 1) + 3;
    gfx_text(x, y, buf, C_TEXT, FONT_SMALL, 1, 0);
    if (fs_info()->mounted) {
        x -= gfx_text_width("SD", FONT_SMALL, 1) + 6;
        gfx_text(x, y, "SD", C_DIM, FONT_SMALL, 1, 0);
    }
    if (torch_get()) {                  /* a lit square */
        x -= 8;
        gfx_fill(x, (UI_STATUS_H - 4) / 2, 4, 4, C_WARN);
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
    gfx_fill(0, UI_TOP, LCD_WIDTH, UI_CONTENT_H, C_BG);
    if (overlay && under->draw)         /* a dialog over the screen under it */
        AS_OWNER(under, under->draw(under));
    if (s->draw)
        AS_OWNER(s, s->draw(s));
    draw_bars(s, overlay ? under->title : s->title);
    draw_status();
    lcd_update();
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
    fs_sync();
    thread_sleep(300);
    arch_power_off();
}

void ui_reboot(void)
{
    splash("Restarting");
    klog("reboot to the boot menu");
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
        keylight_set(true);
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

static void on_key(const key_event_t *ev, uint32_t now)
{
    os_status.keys++;
    os_status.last_key = ev->code | (uint32_t)ev->down << 8;
    last_input = now;

    if (ev->down && light) {            /* the first key only wakes */
        wake();
        swallow_key = ev->code;
        return;
    }
    if (!ev->down && ev->code == swallow_key) {
        swallow_key = 0;
        return;
    }
    if (ev->code == KEY_0)
        zero_down = ev->down ? now : 0;
    if (ev->code == KEY_END) {
        if (ev->down) {
            end_down = now;
        } else if (end_down) {
            end_down = 0;
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

static void every_second(uint32_t now)
{
    static unsigned n;
    uint32_t timeout = (uint32_t)ui_settings.screen_timeout_s * 1000u;


    if (n++ % 30 == 0) {                /* the ADC is slow: twice a minute */
        charging = charger_present();
        bat_mv = battery_mv();
        os_status.battery_mv = (uint32_t)bat_mv;
    }
    os_status.ticks = now;
    os_status.idle_permille = kernel_idle_permille();
    os_status.cpu_mhz = (uint32_t)clock_mhz();
    os_status.heap_free = heap_free_bytes();
    os_status.stray_irqs = irq_stray_count;

    if (timeout && light == 0 && now - last_input > timeout)
        set_light(1);
    else if (timeout && light == 1 && now - last_input > timeout + DARK_AFTER_MS)
        set_light(2);
    dirty = true;                       /* the clock */
}

void ui_run(screen_t *home)
{
    key_event_t ev;

    settings_load();
    cpufreq_set_mode(ui_settings.cpu_max ? CPUFREQ_MAX : CPUFREQ_AUTO);
    ui_apply_brightness();
    keylight_set(true);
    last_input = kernel_ticks();
    next_second = last_input;
    ui_push(home);

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
        if (buzz_until && wait > BUZZ_MS)
            wait = BUZZ_MS;
        if (dirty && light < 2)       /* dark: drawn on wake, do not spin */
            wait = 0;

        if (keypad_get(&ev, wait) == WAIT_OK)
            on_key(&ev, kernel_ticks());
        now = kernel_ticks();

        if (held_key && (int32_t)(now - next_repeat) >= 0) {
            key_event_t rep = { held_key, 1, now };
            next_repeat = now + REPEAT_EVERY;
            dispatch(&rep);
        }
        if (end_down && now - end_down >= END_HOLD_MS)
            ui_power_off();
        if (zero_down && now - zero_down >= TORCH_HOLD_MS) {
            zero_down = 0;              /* one toggle per press */
            torch_set(!torch_get());
            klog("torch %s", torch_get() ? "on" : "off");
            vibrator_set(4);
            buzz_until = now + BUZZ_MS;
            dirty = true;
        }
        if (buzz_until && (int32_t)(now - buzz_until) >= 0) {
            buzz_until = 0;
            vibrator_set(0);
        }
        if ((int32_t)(now - next_second) >= 0) {
            next_second = now + 1000;
            every_second(now);
        }
        if ((int32_t)(now - next_tick) >= 0) {
            screen_t *s = stack[depth - 1];

            next_tick = now + tick_period(s);
            if (s->tick) {
                AS_OWNER(s, s->tick(s));
                dirty = true;
            }
        }
        if (collect) {
            collect = false;
            app_collect(stack, depth);
        }
        if (dirty && light < 2)
            redraw();
    }
}
