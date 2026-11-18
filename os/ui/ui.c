/* B310E OS — the UI thread: screen stack, bars, keys, backlight, settings.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/app.h"
#include "os/arch.h"
#include "os/db.h"
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
#define KILL_AFTER_MS  1500             /* END held on an app: asked, then this long */
#define TORCH_HOLD_MS  1000            /* hold 0: torch on/off (as in Rockbox) */
#define BUZZ_MS        80
#define TACTILE_MS     30                /* a key's feedback buzz */
#define MUSIC_MS       50                /* the music player's refill */
#define DIM_PERCENT    8
#define DARK_AFTER_MS  10000            /* after dimming */
#define OLD_SETTINGS   OS_DIR "/settings.txt"   /* before the store: read once */

ui_settings_t ui_settings = {
    60, 30, 0, 1, 0, 7 * 60,
    VOL_DEFAULTS,
    1, 0,
    0, 1,                            /* UTC until set; the clock from the network */
    0,                               /* developer mode off */
    1,                               /* system sleep in dark idle */
    300,                             /* UI_MODE_AWAKE: 5 minutes with no key */
    900,                             /* Music: 15 minutes playing nothing */
    1,                               /* word completion in the text editor */
};

static screen_t *stack[MAX_DEPTH + 1]; /* one more for an alarm, whatever is open */
static int depth;
static bool dirty;
static uint32_t last_key;                /* for UI_MODE_AWAKE's limit */
static uint32_t last_input, end_down, next_repeat, next_second, next_tick, next_music;
static bool end_in_game;                /* END went down on a raw-key screen */
static bool collect;                    /* an app's screen left the stack */
static uint32_t zero_down;
static uint32_t center_down;            /* Center held on a panel host */
static screen_t *panel_hosts[4];
static int n_hosts;
static struct { screen_t *s; unsigned flags; } modes[4];   /* ui_screen_mode */

#define PANEL_HOLD_MS  600
#define LONG_MS        600              /* UI_MODE_HOLD_KEYS: a long press */
static uint8_t hold_key;                /* UI_MODE_HOLD_KEYS: down, not yet passed on */
static uint32_t hold_down;              /* when; 0: passed on (a press or a long one) */

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

/* Dark idle (no clock hold, nothing playing): the once-a-second work runs
 * this often, and at the minute an alarm rings at. Until 2026-11-17 it ran
 * every second dark too, its charger read an analog-die access that starts
 * the PLL (a 2 ms busy wait) each time. The charger and the battery are
 * read again as the screen wakes. A plug or unplug wakes the UI by the
 * charger's EIC interrupt (keypad.c, keypad_charger_event), so USB that
 * starts by itself on the plug no longer needs a 10 s poll (until
 * 2026-11-18: 6 wakes a minute dark); DARK_TICK_MS stays as the safety net.
 * Each dark pass also kicks the END thread, so its own dark wait (a minute)
 * never ends on a wake of its own: about one wake a minute in all. */
#define DARK_TICK_MS   60000
#define BATTERY_MS     30000            /* the ADC is slow: twice a minute */
#define BATTERY_DARK_MS 300000          /* dark: every 5 minutes (and on wake) */
/* The keypad light: off when the screen dims or goes dark, and KEYLIGHT_MS
 * after the last key while lit; any key (or the screen waking) lights it
 * again (power audit A1; owner's OK 2026-11-18). */
#define KEYLIGHT_MS    5000
static bool keylit;
static uint32_t keylight_off_at;
#define DRAIN_FIRST_MS 180000           /* the drain log: at 3 minutes (not at boot:
                                         * its load is boot work), then every 10 */
#define DRAIN_MS       600000
static uint32_t next_battery, next_drain = DRAIN_FIRST_MS, next_rtc_save = DRAIN_MS;
static bool audio_was;                  /* background audio at the last pass */

/* OS screens' hooks (not the app API): what a screen shows (ui_screen_shown)
 * and whether its draw() redraws only what changed (ui_screen_partial). A
 * screen freed when popped loses its entry then: its address comes back for
 * the next kzalloc'd screen. 32: the static screens, and a stack of folders,
 * pickers and dialogs over them. */
static struct { screen_t *s; uint32_t (*fn)(screen_t *s); bool partial; } hooks[32];
static uint32_t shown_key, shown_skey;  /* the screen's and the bar's, as drawn */
static bool shown_valid;

static int hook_slot(screen_t *s)
{
    int free_slot = -1;

    for (int i = 0; i < (int)ARRAY_SIZE(hooks); i++) {
        if (hooks[i].s == s)
            return i;
        if (!hooks[i].s && free_slot < 0)
            free_slot = i;
    }
    if (free_slot >= 0)
        hooks[free_slot].s = s;
    return free_slot;                   /* full: the screen is drawn as before */
}

void ui_screen_shown(screen_t *s, uint32_t (*shown)(screen_t *s))
{
    int i = hook_slot(s);

    if (i >= 0)
        hooks[i].fn = shown;
}

void ui_screen_partial(screen_t *s)
{
    int i = hook_slot(s);

    if (i >= 0)
        hooks[i].partial = true;
}

static void hook_forget(const screen_t *s)
{
    for (unsigned i = 0; i < ARRAY_SIZE(hooks); i++)
        if (hooks[i].s == s) {
            hooks[i].s = NULL;
            hooks[i].fn = NULL;
            hooks[i].partial = false;
        }
}

static uint32_t (*shown_of(const screen_t *s))(screen_t *)
{
    for (unsigned i = 0; s && i < ARRAY_SIZE(hooks); i++)
        if (hooks[i].s == s)
            return hooks[i].fn;
    return NULL;
}

static bool is_partial(const screen_t *s)
{
    for (unsigned i = 0; i < ARRAY_SIZE(hooks); i++)
        if (hooks[i].s == s)
            return hooks[i].partial;
    return false;
}

/* Until 2026-11-18 the UI redrew after every tick() of the top screen, whatever
 * it did: the text editor ten times a second for a cursor that had not moved
 * (158 of 194 frames unchanged, 80% of a typing session's instructions), the
 * Stopwatch ten times a second at a stopped 00:00.0. Now a tick asks for its
 * frame (ui_redraw) when it changed something; only an app built for API 2.18
 * or older, whose tick may count on the old way, still gets one after each.
 * Its tick is in its image (an OS screen it opened, the editor, is not). */
static bool tick_redraws(const screen_t *s)
{
    uint32_t pc = (uint32_t)(uintptr_t)s->tick, base, size;
    const char *name;

    for (int i = 0; app_span(i, &name, &base, &size); i++)
        if (pc - base < size)
            return ((const app_info_t *)(uintptr_t)base)->api_minor < 19;
    return false;
}

static uint32_t shown_by_tick(screen_t *s)
{
    return 0;                           /* its tick and its keys ask for frames */
}

/* The hook second_redraw goes by: the screen's own, or for a screen whose
 * tick asks for its frames, none needed (the bar's changes still show) */
static uint32_t (*shown_fn(screen_t *s))(screen_t *)
{
    uint32_t (*fn)(screen_t *) = shown_of(s);

    if (!fn && s->tick && !tick_redraws(s))
        fn = shown_by_tick;
    return fn;
}

/* ---- the frame kept in lcd_fb (a back buffer: what was drawn stays) ------- */

/* lcd_fb holds kept_s's last frame (over kept_u, its overlay's screen, whose
 * shown key was kept_ukey) with nothing drawn since: a partial screen's
 * draw() may then redraw only what changed. Anything else that draws, or may
 * have changed what a screen shows without its knowing, drops it. */
static bool kept;
static screen_t *kept_s, *kept_u;
static uint32_t kept_ukey;
static bool partial_now;                /* during draw(): ui_partial() */
static bool base_drawn;                 /* this frame's content area was cleared */
static screen_t *draw_u;                /* the overlay's screen being drawn */

/* The bars as drawn last (copies: a screen may reuse a buffer for a label) */
static char bar_text[4][40];
static bool bar_ok[4];
static uint32_t status_drawn;

static volatile uint32_t drop_gen;     /* ui_refresh may come from another thread */

static void redraw_all(void)
{
    dirty = true;
    kept = false;
    drop_gen++;
}

/* ---- settings ------------------------------------------------------------ */

/* The settings are "ui.<name>" in the store (os/db.h), every name in this
 * table. A name the store lacks keeps its default; one the table lacks is
 * ignored, so stores written by older and newer builds both load. Until
 * 2026-11-17 they were "name=value" lines in settings.txt, read once into the
 * store on the first boot with it (the file stays on the card). */
static const struct {
    const char *name;
    int *v;
    int min, max;
} setting_table[] = {
    { "brightness", &ui_settings.brightness, 5, 100 },
    { "screen_timeout", &ui_settings.screen_timeout_s, 0, 3600 },
    { "cpu_max", &ui_settings.cpu_max, 0, 1 },
    { "deep_idle", &ui_settings.deep_idle, 0, 1 },
    { "chip_sleep", &ui_settings.chip_sleep, 0, 1 },
    { "awake_limit_s", &ui_settings.awake_limit_s, 1, 86400 },
    { "player_idle_s", &ui_settings.player_idle_s, 0, 86400 },
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
    { "dev_mode", &ui_settings.dev_mode, 0, 1 },
    { "completion", &ui_settings.completion, 0, 1 },
};

static void settings_import(void)
{
    char buf[512], *p;
    FIL f;
    UINT n = 0;

    if (f_open(&f, OLD_SETTINGS, FA_READ) != FR_OK)
        return;
    f_read(&f, buf, sizeof(buf) - 1, &n);
    f_close(&f);
    buf[n] = 0;
    for (p = buf; *p; ) {
        char *line = p, *eq;
        bool neg;
        int v = 0;

        while (*p && *p != '\n')
            p++;
        if (*p)
            *p++ = 0;
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq++ = 0;
        neg = *eq == '-';
        if (neg)
            eq++;
        while (*eq >= '0' && *eq <= '9')
            v = v * 10 + (*eq++ - '0');
        if (neg)                        /* a time zone west of Greenwich */
            v = -v;
        for (unsigned i = 0; i < ARRAY_SIZE(setting_table); i++)
            if (!strcmp(line, setting_table[i].name) && v >= setting_table[i].min &&
                v <= setting_table[i].max)
                *setting_table[i].v = v;
    }
    klog("settings: %s read into the store", OLD_SETTINGS);
}

/* What the store holds for each setting as far as we know (read or written
 * last): ui_settings_save writes only the ones that differ. Writing all 20
 * was 20 snprintf and 20 key-list walks a toggle (about 1 M of the 6.65 M of
 * 20 Settings toggles, bench 2026-11-17). */
static int stored[ARRAY_SIZE(setting_table)];
static bool stored_known;

static void settings_from_db(void)
{
    char key[32];

    for (unsigned i = 0; i < ARRAY_SIZE(setting_table); i++) {
        int v;

        snprintf(key, sizeof(key), "ui.%s", setting_table[i].name);
        v = db_get_int(key, *setting_table[i].v);
        stored[i] = v;                  /* out of range too: written over at a save */
        if (v >= setting_table[i].min && v <= setting_table[i].max)
            *setting_table[i].v = v;
    }
    stored_known = true;
}

/* Only what changed is written, a second or two later (the store batches). */
void ui_settings_save(void)
{
    char key[32];

    for (unsigned i = 0; i < ARRAY_SIZE(setting_table); i++) {
        if (stored_known && stored[i] == *setting_table[i].v)
            continue;
        snprintf(key, sizeof(key), "ui.%s", setting_table[i].name);
        if (db_set_int(key, *setting_table[i].v) == DB_OK)
            stored[i] = *setting_table[i].v;    /* else tried again at the next */
    }
}

static void settings_load(void)
{
    if (!db_get_bool("import.settings", false)) {
        settings_import();
        ui_settings_save();
        db_set_bool("import.settings", true);
    }
    settings_from_db();
}

/* "ui." changed in the store, maybe not by us (the shell's db set, an import):
 * what the screen and the clock use is taken up. */
static void settings_changed(void *ctx)
{
    ui_settings_t was = ui_settings;

    settings_from_db();
    if (ui_settings.brightness != was.brightness && light == 0)
        ui_apply_brightness();
    if (ui_settings.keylight != was.keylight)
        ui_keylight_touch();
    if (ui_settings.cpu_max != was.cpu_max)
        cpufreq_set_mode(ui_settings.cpu_max ? CPUFREQ_MAX : CPUFREQ_AUTO);
    if (ui_settings.deep_idle != was.deep_idle)
        kernel_set_tickless(ui_settings.deep_idle);
    if (ui_settings.chip_sleep != was.chip_sleep)
        sleep_idle_enable(ui_settings.chip_sleep);
    /* our own save comes back here too, with nothing new: the frame the key
     * drew stays; a value set elsewhere (the shell) is drawn in full */
    if (memcmp(&ui_settings, &was, sizeof(was)))
        redraw_all();
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

static bool urgent_push;

void ui_push_urgent(screen_t *s)
{
    urgent_push = true;
    ui_push(s);
    urgent_push = false;
}

void ui_push(screen_t *s)
{
    if (!s || depth >= MAX_DEPTH + (urgent_push ? 1 : 0)) {
        /* not pushed: released as ui_pop would, or it leaks with its data */
        if (s && s->free_on_pop) {
            app_t *a = s->app ? s->app : app_self();

            if (a) {
                collect = true;
                app_forget(a, s);
            }
            hook_forget(s);
            kfree(s);
        }
        return;
    }
    held_key = 0;                       /* a held key's repeat is not the new screen's */
    hold_key = 0;
    hold_down = 0;
    if (depth && top_mode() & UI_MODE_RAW_KEYS)
        raw_release_all(stack[depth - 1], kernel_ticks());  /* a game lets go of its keys */
    if (!s->app)
        s->app = app_self();
    stack[depth++] = s;
    if (s->enter)
        AS_OWNER(s, s->enter(s));
    next_tick = kernel_ticks() + tick_period(s);
    publish();
    redraw_all();
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
            redraw_all();
            return;
        }
        if (!modes[i].s && free_slot < 0)
            free_slot = i;
    }
    if (flags && free_slot >= 0) {
        modes[free_slot].s = s;
        modes[free_slot].flags = flags;
        redraw_all();
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
    held_key = 0;                       /* nor the screen's under it */
    hold_key = 0;
    hold_down = 0;
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
    if (s->free_on_pop) {
        hook_forget(s);
        kfree(s);
    }
    next_tick = kernel_ticks() + tick_period(stack[depth - 1]);
    publish();
    redraw_all();
}

void ui_home(void)
{
    while (depth > 1)
        ui_pop();
}

/* Asked from anywhere (a notification, a tick, an app): whatever changed may
 * be under a dialog too, so the whole frame. A key's own redraw (dispatch)
 * lets a partial screen redraw only what it finds changed. */
void ui_redraw(void)
{
    redraw_all();
}

void ui_redraw_changed(void)
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
    redraw_all();
    keypad_inject(0, false);
}

int ui_depth(void)
{
    return depth;
}

/* ---- drawing --------------------------------------------------------------- */

/* 0 speaker, 1 headphones, 2 headset with a mic, 3 muted. Muted is the level
 * of what is playing, or with nothing playing the music's, or the radio's
 * own mute. */
static int audio_status_icon(void)
{
    audio_headset_info_t h;

    if (volume_level(volume_current()) == 0 || fm_muted())
        return 3;
    if (audio_forced_speaker() || !audio_headphones())
        return 0;
    audio_headset_info(&h);
    return h.buttons ? 2 : 1;
}

/* t: the time status_key had, so the bar and its key agree at a minute's turn */
static void draw_status(const rtc_datetime_t *t)
{
    char buf[16];
    int pct, sh = gfx_text_height(FONT_SMALL, 1), y = (UI_STATUS_H - sh) / 2;
    int x = LCD_WIDTH - UI_PAD - 17;

    gfx_fill(0, 0, LCD_WIDTH, UI_STATUS_H, C_BAR);
    snprintf(buf, sizeof(buf), "%02d:%02d", t->hour, t->min);
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
    {                                   /* where the sound goes, or muted */
        int a = audio_status_icon();
        static const icon_t *const icons[] = {
            &icon_speaker, &icon_headphones, &icon_headset, &icon_speaker_off,
        };

        os_status.audio_icon = (uint32_t)a;
        x -= ICON_SIZE + 2;
        gfx_icon(x, 0, icons[a], a == 3 ? C_WARN : C_DIM);
    }
    {                                   /* the notifications' icons after the time */
        int ix = UI_PAD + 2 + gfx_text_width("00:00", FONT_SMALL, 1) + 5;
        notify_t *n;

        /* the icons have their own margin: "chg" left the USB icon out by a
         * pixel when this asked for 2 more */
        for (int i = 0; (n = notify_get(i)) && ix + ICON_SIZE <= x; i++) {
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

/* The title and soft-key labels as drawn: false if one differs (or is too
 * long to keep), and then kept as they are now */
static bool bars_same(const char *const now[4])
{
    bool same = true;

    for (int i = 0; i < 4; i++) {
        const char *t = now[i] ? now[i] : "";
        size_t n = strlen(t);

        if (!bar_ok[i] || n >= sizeof(bar_text[i]) || strcmp(bar_text[i], t))
            same = false;
        bar_ok[i] = n < sizeof(bar_text[i]);
        if (bar_ok[i])
            memcpy(bar_text[i], t, n + 1);
    }
    return same;
}

/* The content area cleared and the screen under an overlay drawn: before
 * draw(), or from a partial draw() that finds it must draw everything */
static void draw_base(void)
{
    partial_now = false;
    base_drawn = true;
    gfx_fill(0, UI_TOP, LCD_WIDTH, UI_CONTENT_H, C_BG);
    if (draw_u && draw_u->draw)         /* a dialog over the screen under it */
        AS_OWNER(draw_u, draw_u->draw(draw_u));
}

bool ui_partial(void)
{
    return partial_now;
}

void ui_draw_base(void)
{
    if (partial_now)
        draw_base();
}

static uint32_t status_key(const rtc_datetime_t *t);
static void shown_note(screen_t *s, screen_t *under, const rtc_datetime_t *t, uint32_t skey);

static bool first_frame = true;         /* redraw's first push: lcd_update() */

static void redraw(void)
{
    screen_t *s = stack[depth - 1], *under = depth > 1 ? stack[depth - 2] : NULL;
    bool overlay = s->overlay && under;
    uint32_t gen = drop_gen, skey;
    const char *labels[4];
    rtc_datetime_t t;

    if (light == 2)                     /* asleep: drawn again on wake */
        return;
    dirty = false;
    if (app_reloading()) {              /* an app between two loads: not the
                                         * screen under it for a moment */
        kept = false;
        gfx_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
        gfx_text_centered(LCD_HEIGHT / 2 - 4, RELOADING, C_DIM, FONT_SMALL, 1);
        lcd_update();
        return;
    }
    if (top_mode() & UI_MODE_FULL) {    /* the whole LCD is the screen's */
        kept = false;
        if (s->draw)
            AS_OWNER(s, s->draw(s));
        lcd_update();
        return;
    }
    /* The same screen over the same one as the frame in lcd_fb, nothing in
     * between: a partial screen redraws what changed (a list the two rows a
     * key moved, a dialog its buttons), and the bars only if they changed.
     * Under a dialog only a screen that says what it shows, unchanged. */
    draw_u = overlay ? under : NULL;
    partial_now = kept && kept_s == s && kept_u == draw_u && is_partial(s);
    if (partial_now && draw_u) {
        uint32_t (*ufn)(screen_t *) = shown_of(draw_u);

        partial_now = ufn && ufn(draw_u) == kept_ukey;
    }
    base_drawn = false;
    if (!partial_now)
        draw_base();
    if (s->draw)
        AS_OWNER(s, s->draw(s));
    partial_now = false;
    labels[0] = overlay ? under->title : s->title;
    labels[1] = s->left;
    labels[2] = s->center;
    labels[3] = s->right;
    /* a full draw may run past the content area (the quick panel's rows):
     * the bars go over it as before */
    rtc_get(&t);                        /* carry-safe, as every_second's */
    skey = status_key(&t);
    /* The status bar's icons are 12 px at y 0 in an 11 px bar: their last
     * row lies on the title bar. Drawn alone, the status blended that row
     * over its own last blend (the ui-tour's radio icon, 2026-11-18): with
     * the status, the bars go first, as in a full draw. */
    if (!bars_same(labels) || base_drawn || skey != status_drawn)
        draw_bars(s, labels[0]);
    if (skey != status_drawn || base_drawn) {
        draw_status(&t);
        status_drawn = skey;
    }
    /* Everything here drew through gfx_* (which marks its rows), so only
     * those rows are compared; the first frame after boot compares them
     * all, whatever wrote lcd_fb before the UI (main.c, the console). */
    if (first_frame) {
        first_frame = false;
        lcd_update();
    } else {
        lcd_update_drawn();
    }
    kept = gen == drop_gen;            /* not if dropped meanwhile (ui_refresh) */
    kept_s = s;
    kept_u = draw_u;
    if (draw_u) {
        uint32_t (*ufn)(screen_t *) = shown_of(draw_u);

        kept_ukey = ufn ? ufn(draw_u) : 0;
    }
    draw_u = NULL;
    shown_note(s, under, &t, skey);
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
    kept = false;
    lcd_wake();
    gfx_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
    gfx_text_centered(LCD_HEIGHT / 2 - 8, text, C_TEXT, FONT_LARGE, 1);
    lcd_update();
}

/* END held: the app on top closes, and the phone stays on. Its screens go, which
 * asks it to end (a game saves); if it has not after KILL_AFTER_MS, it is ended.
 * No app on the stack: false (the phone powers off). */
static app_t *closing;
static uint32_t closing_at;

static bool close_top_app(uint32_t now)
{
    app_t *a = NULL;
    int lowest = 0;

    for (int i = depth - 1; i >= 1; i--)
        if (stack[i]->app) {
            if (!a)
                a = stack[i]->app;
            if (stack[i]->app == a)
                lowest = i;
        }
    if (!a)
        return false;
    klog("ui: END held: closing %s", app_name(a));
    closing = a;
    closing_at = now;
    while (depth > lowest)
        ui_pop();
    collect = true;
    return true;
}

static void closing_tick(uint32_t now)
{
    if (!closing)
        return;
    if (!app_is_loaded(closing))
        closing = NULL;
    else if (now - closing_at >= KILL_AFTER_MS) {
        /* only what did not end: Music, playing on from its notification, is not
         * hung, and dropping that left it playing with no way to stop it */
        if (thread_count_owned(closing))
            app_kill(closing);
        closing = NULL;
        collect = true;
    }
}

void ui_power_off(void)
{
    splash("Powering off");
    klog("power off");
    wav_stop();                         /* a recording's file: finished */
    volume_save();
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
    klog("reboot: hard reset");
    wav_stop();
    volume_save();
    rtc_save();
    fs_sync();
    thread_sleep(300);
    backlight_set(0);
    keylight_set(false);
    /* Back to this OS unless something else was asked: without a request
     * the loader leaves a reset to the stock firmware. */
    if (arch_boot_loader() && !arch_boot_pending())
        arch_boot_request(BOOT_REQ_OS, NULL);
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

/* UI_MODE_HOLD_KEYS: the keys a text editor gives a long press of their own */
static bool holdable(uint8_t code)
{
    return (code >= KEY_0 && code <= KEY_9) || code == KEY_STAR || code == KEY_HASH;
}

/* The key waited on reaches the screen: as a press, or held long */
static void hold_pass(uint32_t now)
{
    key_event_t ev = { hold_key, 1, now, 0 };

    hold_down = 0;
    dispatch(&ev);
}

static void keylight_touch(uint32_t now)
{
    keylight_off_at = now + KEYLIGHT_MS;
    if (!keylit && ui_settings.keylight) {
        keylight_set(true);
        keylit = true;
    }
}

static void keylight_off(void)
{
    if (keylit) {
        keylight_set(false);
        keylit = false;
    }
}

/* the setting changed (Settings): on now for KEYLIGHT_MS, or off */
void ui_keylight_touch(void)
{
    if (ui_settings.keylight && light == 0) {
        keylight_touch(kernel_ticks());
    } else {
        keylight_set(false);
        keylit = false;
    }
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
        keylit = false;
        lcd_sleep();
        audio_headset_idle(true);
        return;
    }
    if (light == 2) {
        audio_headset_idle(false);
        keypad_kick();                  /* the headset's buttons back now */
        light = level;
        charging = charge_state();      /* dark it was read every minute only */
        next_second = kernel_ticks();   /* the rest (battery ...) this pass */
        next_battery = next_second;     /* dark: read every 5 minutes only */
        redraw();                       /* the frame the panel wakes to */
        lcd_wake();
    }
    light = level;
    if (level == 1) {
        keylight_off();
        backlight_set(DIM_PERCENT);
    } else {
        keylight_touch(kernel_ticks());
        ui_apply_brightness();
    }
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

/* At boot, from ui_run: a thread that lives for ever, so its stack sits at
 * the bottom of the heap. Made at the first buzz (2026-11-18, the phone) it
 * came after Music's big blocks and stayed between them: ~500 KB free under
 * it that a large block could no longer use. */
static void buzz_start(void)
{
    if (buzz_thread)
        return;
    sem_init(&buzz_sem, 0);
    mutex_init(&buzz_lock);
    buzz_thread = thread_create("buzz", PRIO_NORMAL + 3, 768, buzz_main, NULL);
}

void ui_buzz(int level, uint32_t ms)
{
    if (level <= 0 || !ms)
        return;
    if (!buzz_thread) {                 /* failed at boot: once more now */
        buzz_start();
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
    last_input = last_key = kernel_ticks();
    wake();
}

bool ui_is_dark(void)
{
    return light == 2;
}

void ui_force_dark(void)
{
    last_input = last_key = kernel_ticks() - 0x40000000u;
    if (light != 2)
        set_light(2);
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
            key_event_t up = { (uint8_t)code, 0, now, 0 };

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
    if (alarm_ringing)                  /* over it, its tick stops: the buzz stays on
                                         * and it never times out */
        return true;
    held_key = swallow_key = 0;
    center_down = zero_down = 0;
    hold_key = 0;                       /* a * or # the editor waited on */
    hold_down = 0;
    if (top_mode() & UI_MODE_RAW_KEYS)
        raw_release_all(s, now);        /* the first of the two, and the rest */
    if (s == ui_panel() || s == ui_sound()) {
        if (s == ui_sound())
            ui_pop();
        if (stack[depth - 1] == ui_panel())
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
    last_input = last_key = now;
    if (ev->down && light == 0)
        keylight_touch(now);            /* dim or dark: wake() lights it */

    if (app_reloading()) {              /* the keys are not for the screen under it */
        if (ev->code == KEY_END && !ev->down) {
            end_down = 0;
            end_in_game = false;
        }
        held_key = swallow_key = 0;
        center_down = zero_down = 0;
        hold_key = 0;
        hold_down = 0;
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
        if (ev->code == KEY_END)        /* held on: still the power off */
            end_down = now ? now : 1;
        return;
    }
    if (!ev->down && ev->code == swallow_key) {
        swallow_key = 0;
        if (ev->code == KEY_END)
            end_down = 0;
        if (ev->code == held_key)
            held_key = 0;
        return;
    }
    if (ev->down && ui_settings.vol[VOL_TACTILE]) {
        /* tactile feedback: a short buzz a press, at the chosen strength */
        static const uint8_t strength[4] = { 0, 2, 4, 7 };

        ui_buzz(strength[ui_settings.vol[VOL_TACTILE] & 3], TACTILE_MS);
    }
    if (ev->down && hold_down && ev->code != hold_key)
        hold_pass(now);                 /* the one still down was a press */
    if ((top_mode() & UI_MODE_HOLD_KEYS) && holdable(ev->code)) {
        if (ev->down) {
            hold_key = ev->code;
            hold_down = now ? now : 1;
        } else if (ev->code == hold_key) {
            if (hold_down)              /* let go before the hold: a press */
                hold_pass(now);
            hold_key = 0;
        }
        return;                         /* 0 held is not the torch here */
    }
    if (ev->code == KEY_0)
        zero_down = ev->down ? now : 0;
    if (ev->code == KEY_CENTER && (center_down || on_host())) {
        if (ev->down) {
            center_down = now ? now : 1;
        } else if (center_down) {       /* let go before the hold: a press */
            key_event_t press = { KEY_CENTER, 1, now, 0 };

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
    static uint32_t last_ms[CLOCK_LEVELS], last_irqs, last_sleeps, last_starts, last_now, last_adi_errors;
    uint32_t pct[CLOCK_LEVELS], span = 0, irqs = irq_count, sleeps = sleep_idle_count;
    uint32_t starts = clock_pll_starts();

    for (int i = 0; i < CLOCK_LEVELS; i++) {
        uint32_t ms = clock_level_ms(i);

        pct[i] = ms - last_ms[i];
        last_ms[i] = ms;
        span += pct[i];
    }
    for (int i = 0; i < CLOCK_LEVELS; i++)
        pct[i] = span ? (uint32_t)((uint64_t)pct[i] * 100 / span) : 0;
    rtc_get(&t);
    klog("battery: %d mV %d%%%s, load %lu.%lu%%, %d MHz, 26/104/208 MHz %lu/%lu/%lu%%, "
         "holds %d, %lu irqs/s, %lu sleeps/s, %lu PLL starts/s, screen %s, rtc %02d:%02d:%02d, up %lu s",
         bat_mv, battery_percent(bat_mv),
         charging == CHARGE_CHARGING ? ", charging" : charging == CHARGE_DONE ? ", charged" : "",
         (unsigned long)(load / 10), (unsigned long)(load % 10), clock_mhz(),
         (unsigned long)pct[0], (unsigned long)pct[1], (unsigned long)pct[2], cpufreq_holds(),
         (unsigned long)((irqs - last_irqs) / ((now - last_now) / 1000u + 1u)),
         (unsigned long)((sleeps - last_sleeps) / ((now - last_now) / 1000u + 1u)),
         (unsigned long)((starts - last_starts) / ((now - last_now) / 1000u + 1u)),
         light == 0 ? "on" : light == 1 ? "dim" : "dark", t.hour, t.min, t.sec,
         (unsigned long)(now / 1000));
    if (adi_read_errors != last_adi_errors) {
        klog("adi: %lu read errors so far", (unsigned long)adi_read_errors);
        last_adi_errors = adi_read_errors;
    }
    last_irqs = irqs;
    last_sleeps = sleeps;
    last_starts = starts;
    last_now = now;
}

/* What the status bar shows, as one number: it changes when the bar would */
static uint32_t status_key(const rtc_datetime_t *t)
{
    uint32_t k = (uint32_t)(t->day * 1440 + t->hour * 60 + t->min);
    notify_t *n;

    k = k * 31u + (uint32_t)battery_percent(bat_mv);
    k = k * 31u + (uint32_t)charging;
    k = k * 31u + (uint32_t)(uintptr_t)usb_auto_label();
    k = k * 31u + (uint32_t)torch_get();
    k = k * 31u + (uint32_t)audio_status_icon();
    for (int i = 0; (n = notify_get(i)); i++)  /* posts redraw; an update may change an icon */
        k = k * 31u + (uint32_t)(uintptr_t)n->icon + 1u;
    return k;
}

/* Lit: the frame again only if the bar or the top screen changed, for a
 * screen that says what it shows (ui_screen_shown); the others every second
 * as before. On Home that was a whole redraw a second (about 7 ms of CPU at
 * 104 MHz) for lcd_update to find nothing changed, for a clock in minutes. */
/* The screen's shown key now (with the one under a dialog); false: it has
 * none, and is drawn every second. A screen under a dialog goes by its own
 * hook only: its tick does not run while covered, and it was redrawn (live
 * figures, a running clock) each second. */
static bool shown_now(screen_t *s, screen_t *under, uint32_t *key)
{
    uint32_t (*fn)(screen_t *) = shown_fn(s), (*ufn)(screen_t *) = NULL;

    if (s->overlay && under && !(ufn = shown_of(under)))
        fn = NULL;                      /* the screen under a dialog is drawn too */
    if (!fn || app_reloading() || (top_mode() & UI_MODE_FULL))
        return false;
    *key = fn(s);
    if (ufn)
        *key = *key * 31u + ufn(under);
    return true;
}

/* After a frame: what it showed. Compared with the last second's key (as
 * until 2026-11-18) a frame a key had drawn was drawn again at the next
 * second, as the selection was in the key. */
static void shown_note(screen_t *s, screen_t *under, const rtc_datetime_t *t, uint32_t skey)
{
    shown_valid = shown_now(s, under, &shown_key);
    shown_skey = skey;
}

static void second_redraw(const rtc_datetime_t *t)
{
    screen_t *s = stack[depth - 1], *under = depth > 1 ? stack[depth - 2] : NULL;
    uint32_t key, skey;

    if (!shown_now(s, under, &key)) {
        shown_valid = false;
        redraw_all();                   /* as before: all of it, every second */
        return;
    }
    skey = status_key(t);
    if (!shown_valid || key != shown_key)
        redraw_all();                   /* the screen says it changed */
    else if (skey != shown_skey)
        dirty = true;                   /* the bar only: a partial screen keeps its frame */
    shown_key = key;
    shown_skey = skey;
    shown_valid = true;
}

/* The next run of every_second dark: DARK_TICK_MS on, or just past the minute
 * the next alarm rings at (alarm_check rings it at the minute). */
static uint32_t dark_next(uint32_t now, const rtc_datetime_t *t)
{
    uint32_t wait = DARK_TICK_MS, in, usb = usb_auto_due_ms(now);

    if (usb < wait)                     /* a bench ends: USB back on time */
        wait = usb + 50;

    if (alarm_next(&in) >= 0 && in >= 1 && in <= 24u * 60) {
        uint32_t at = ((in - 1) * 60u + (uint32_t)(60 - t->sec)) * 1000u + 50u;

        if (at < wait)
            wait = at;
    }
    return now + wait;
}

/* Only what needs the second waits for it: a clock hold (codec, USB, an app,
 * a card transfer) keeps the CPU from the chip sleep anyway, and sound, a
 * ringing or snoozed alarm (its snooze is in ms) or an app closing tick at 1 s */
static bool dark_idle(void)
{
    return light == 2 && !cpufreq_holds() && !background_audio() && !alarm_ringing &&
           alarm_snooze_min < 0 && !closing;
}

static void every_second(uint32_t now)
{
    uint32_t timeout = (uint32_t)ui_settings.screen_timeout_s * 1000u;
    rtc_datetime_t t;

    rtc_get(&t);                        /* carry-safe: rtc_time can read 11:00 as 11:59 */
    alarm_check(t.hour, t.min);
    usb_auto_tick();
    notify_tick();
    audio_idle_tick();
    sd_idle_tick();
    volume_save();
    charging = charge_state();          /* a plug shows at once, as on Home */
    if ((int32_t)(now - next_battery) >= 0) {
        next_battery = now + (light == 2 ? BATTERY_DARK_MS : BATTERY_MS);
        bat_mv = battery_mv();
        os_status.battery_mv = (uint32_t)bat_mv;
    }
    if ((int32_t)(now - next_drain) >= 0) {
        next_drain = (now / DRAIN_MS + 1) * DRAIN_MS;
        drain_log(now);
    }
    if ((int32_t)(now - next_rtc_save) >= 0) {
        next_rtc_save = now + DRAIN_MS;
        rtc_save();
    }
    os_status.ticks = now;
    os_status.idle_permille = kernel_idle_permille();
    os_status.cpu_mhz = (uint32_t)clock_mhz();
    os_status.heap_free = heap_free_bytes();
    os_status.stray_irqs = irq_stray_count;

    /* Signed: what ran above (the alarm ringing, an app's notification) may
     * have woken the screen with kernel_ticks(), later than now; unsigned,
     * now - last_input wrapped to ~4e9 and the ringing alarm's screen was
     * dimmed in the same pass and stayed dim (the phone, 2026-11-18). */
    if ((top_mode() & UI_MODE_AWAKE) &&
        (int32_t)(now - last_key) < (int32_t)((uint32_t)ui_settings.awake_limit_s * 1000u))
        last_input = now;               /* a game being played: no timeout */
    if (timeout && light == 0 && (int32_t)(now - last_input) > (int32_t)timeout)
        set_light(1);
    else if (timeout && light == 1 &&
             (int32_t)(now - last_input) > (int32_t)(timeout + DARK_AFTER_MS))
        set_light(2);
    if (light < 2)
        second_redraw(&t);
    if (dark_idle()) {
        next_second = dark_next(now, &t);
        keypad_kick();                  /* its dark wait restarts from this wake */
    }
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
    buzz_start();                       /* low in the heap, before any app */

    settings_load();
    db_watch("ui.", false, settings_changed, NULL);
    volume_load();
    rtc_restore();
    cpufreq_set_mode(ui_settings.cpu_max ? CPUFREQ_MAX : CPUFREQ_AUTO);
    kernel_set_tickless(ui_settings.deep_idle);
    sleep_idle_enable(ui_settings.chip_sleep);
    ui_apply_brightness();
    last_input = kernel_ticks();
    keylight_set(false);                /* the loader may have left it on */
    keylight_touch(last_input);
    next_second = last_input;
    sem_init(&call_done, 0);
    mutex_init(&call_lock);
    ui_push(home);
    redraw();
    klog("boot: home drawn at %lu ms", (unsigned long)kernel_ticks());

    for (;;) {
        uint32_t now = kernel_ticks(), wait;
        bool audio = background_audio();

        /* up to DARK_TICK_MS in dark idle (every_second sets next_second) */
        wait = (int32_t)(next_second - now) > 0 ? next_second - now : 0;
        if (held_key && (int32_t)(next_repeat - now) < (int32_t)wait)
            wait = (int32_t)(next_repeat - now) > 0 ? next_repeat - now : 0;
        /* a screen without tick() has nothing to wake for: Home woke each second */
        if (stack[depth - 1]->tick && (int32_t)(next_tick - now) < (int32_t)wait)
            wait = (int32_t)(next_tick - now) > 0 ? next_tick - now : 0;
        if ((end_down || zero_down || closing) && wait > 100)
            wait = 100;
        if ((center_down || hold_down) && wait > 50)
            wait = 50;
        if (lcd_holding() && lcd_busy_ms() < wait)
            wait = lcd_busy_ms();       /* its clock hold back when done */
        if (keylit && (int32_t)(keylight_off_at - now) < (int32_t)wait)
            wait = (int32_t)(keylight_off_at - now) > 0 ? keylight_off_at - now : 0;
        if (audio && !audio_was) {      /* a sound started: the jack matters again, */
            keypad_kick();              /* and the second's work (audio idle, notifications) */
            if ((int32_t)(next_second - now) > 1000)
                next_second = now + 1000;
        }
        audio_was = audio;
        if (audio && wait > MUSIC_MS)
            wait = MUSIC_MS;
        if (dirty && light < 2) {     /* dark: drawn on wake, do not spin */
            uint32_t busy = lcd_busy_ms();  /* the keys in the meantime, and then
                                             * what is newest, not what was */
            if (busy < wait)
                wait = busy;
        }

        if (keypad_get(&ev, wait) == WAIT_OK && ev.code)
            on_key(&ev, kernel_ticks());
        if (keypad_charger_event())     /* plugged or unplugged: usb_auto_tick now */
            next_second = kernel_ticks();
        lcd_tick();
        if (call_fn)
            serve_call();
        db_dispatch();                  /* the store's watchers: here, on this thread */
        now = kernel_ticks();
        if (background_audio() && (int32_t)(now - next_music) >= 0) {
            next_music = now + MUSIC_MS;    /* on any screen: the play queue, */
            media_queue_tick();             /* and WAV files playing or recording */
            wav_pump();
        }

        if (held_key && (int32_t)(now - next_repeat) >= 0) {
            key_event_t rep = { held_key, 1, now, 1 };
            next_repeat = now + REPEAT_EVERY;
            dispatch(&rep);
        }
        ui_alive_at = now;
        if (keylit && (int32_t)(now - keylight_off_at) >= 0)
            keylight_off();
        if (ui_end_held) {
            ui_end_held = false;
            ui_power_off();
        }
        if (end_down && now - end_down >= END_HOLD_MS) {
            end_down = 0;
            end_in_game = false;
            if (!close_top_app(now))
                ui_power_off();
        }
        closing_tick(now);
        if (center_down && now - center_down >= PANEL_HOLD_MS) {
            center_down = 0;            /* held: the quick panel, not a press */
            ui_buzz(4, 40);
            ui_push(ui_panel());
        }
        if (hold_down && now - hold_down >= LONG_MS) {
            key_event_t ev = { hold_key, 1, now, UI_KEY_LONG };

            hold_down = 0;              /* its release does nothing */
            dispatch(&ev);
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
                bool old_way = tick_redraws(s);     /* before: the tick may pop s */

                AS_OWNER(s, s->tick(s));
                if (old_way)
                    redraw_all();
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
