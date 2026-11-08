/* B310E OS — Settings, by category, and the Power screen.
 *
 * Settings lists the categories; each is a page of rows with the current
 * value at the right. Center on a row opens a picker of its values (a
 * switch row flips at once, an action row does its thing). Every change is
 * saved at once (ui_settings_save) and heard or seen at once.
 *
 *   Display             brightness, screen off after, keypad light
 *   Sound & touch   the volume of music, the alarm, calls, each app; the
 *                       keys' tactile feedback
 *   Power               CPU speed, deep idle
 *   Date & time         (apps/clock.c)
 *   USB                 what a data cable starts in the background
 *   Storage             the card, the cover cache
 *   About
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/audio.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/notify.h"
#include "os/rtc.h"
#include "os/ui.h"
#include "build_info.h"

enum { S_PICK, S_SWITCH, S_ACTION };

typedef struct {
    const char *name;
    const icon_t *icon;
    int kind;
    const char *const *options;         /* S_PICK */
    int n;
    int (*get)(void);                   /* the option's index, or 0/1 */
    void (*set)(int v);                 /* S_PICK, S_SWITCH */
    void (*act)(void);                  /* S_ACTION */
    void (*detail)(char *out, int n);   /* S_ACTION: the text at the right */
} setting_t;

typedef struct {
    const char *title;
    const icon_t *icon;
    const setting_t *rows;
    int n;
    screen_t *(*open)(void);            /* a screen of its own instead */
    void (*act)(void);                  /* or an action */
} category_t;

/* ---- the values --------------------------------------------------------------- */

static const char *const brightness_names[] = {
    "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%",
};
static int get_brightness(void)
{
    int i = (ui_settings.brightness + 5) / 10 - 1;

    return i < 0 ? 0 : i > 9 ? 9 : i;
}
static void set_brightness(int i)
{
    ui_settings.brightness = (i + 1) * 10;
    ui_apply_brightness();
}

static const char *const timeout_names[] = {
    "15 s", "30 s", "1 min", "2 min", "5 min", "10 min", "Never",
};
static const int timeout_values[] = { 15, 30, 60, 120, 300, 600, 0 };
static int get_timeout(void)
{
    for (unsigned i = 0; i < ARRAY_SIZE(timeout_values); i++)
        if (timeout_values[i] == ui_settings.screen_timeout_s)
            return (int)i;
    return 1;
}
static void set_timeout(int i)
{
    ui_settings.screen_timeout_s = timeout_values[i];
}

static int get_keylight(void) { return ui_settings.keylight; }
static void set_keylight(int on)
{
    ui_settings.keylight = on;
    keylight_set(on);
}

static const char *const volume_names[] = {
    "Mute", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10",
};
static int get_vol_music(void) { return volume_level(VOL_MUSIC); }
static void set_vol_music(int v) { volume_set(VOL_MUSIC, v); }
static int get_vol_app(void) { return volume_level(VOL_APP); }
static void set_vol_app(int v) { volume_set(VOL_APP, v); }
static int get_vol_alarm(void) { return volume_level(VOL_ALARM); }
static void set_vol_alarm(int v) { volume_set(VOL_ALARM, v); }
static int get_vol_call(void) { return volume_level(VOL_CALL); }
static void set_vol_call(int v) { volume_set(VOL_CALL, v); }

static const char *const tactile_names[] = { "Off", "Light", "Medium", "Strong" };
static int get_tactile(void) { return volume_level(VOL_TACTILE); }
static void set_tactile(int v)
{
    volume_set(VOL_TACTILE, v);
    if (v) {                            /* a taste of it */
        static const uint8_t strength[4] = { 0, 2, 4, 7 };

        vibrator_set(strength[v]);
        thread_sleep(40);
        vibrator_set(0);
    }
}

static void open_app_volumes(void)
{
    ui_push(ui_sound());
}

static const char *const cpu_names[] = { "Auto", "Always 208 MHz" };
static int get_cpu(void) { return ui_settings.cpu_max; }
static void set_cpu(int v)
{
    ui_settings.cpu_max = v;
    cpufreq_set_mode(v ? CPUFREQ_MAX : CPUFREQ_AUTO);
}

static int get_deep_idle(void) { return ui_settings.deep_idle; }
static void set_deep_idle(int on)
{
    ui_settings.deep_idle = on;
    kernel_set_tickless(on);
}

static void card_detail(char *out, int n)
{
    const fs_info_t *fi = fs_info();

    if (!fi->mounted)
        snprintf(out, (size_t)n, "none");
    else if (fi->free_known)
        snprintf(out, (size_t)n, "%lu MB free", (unsigned long)(fi->free_kib / 1024));
    else
        snprintf(out, (size_t)n, "%lu MB", (unsigned long)(fi->sectors / 2048));
}
static void card_info(void)
{
    const fs_info_t *fi = fs_info();
    char msg[96];

    if (!fi->mounted) {
        ui_message("Card", "No card, or not FAT.");
        return;
    }
    snprintf(msg, sizeof(msg), "%s%s%lu MB\n%lu MB free", fi->label[0] ? fi->label : "",
             fi->label[0] ? "\n" : "", (unsigned long)(fi->sectors / 2048),
             (unsigned long)(fi->free_kib / 1024));
    ui_message("Card", msg);
}

#define ART_CACHE OS_DIR "/cache/art"
static void cache_detail(char *out, int n)
{
    DIR d;
    FILINFO fi;
    int files = 0;

    if (f_opendir(&d, ART_CACHE) == FR_OK) {
        while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
            files++;
        f_closedir(&d);
    }
    snprintf(out, (size_t)n, "%d covers", files);
}
static void cache_clear_yes(void)
{
    DIR d;
    FILINFO fi;
    char path[64];
    int n = 0;

    if (f_opendir(&d, ART_CACHE) != FR_OK)
        return;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        snprintf(path, sizeof(path), ART_CACHE "/%s", fi.fname);
        if (f_unlink(path) == FR_OK)
            n++;
    }
    f_closedir(&d);
    klog("settings: cover cache cleared, %d files", n);
}
static void cache_clear(void)
{
    ui_confirm("Cover cache", "Delete the saved album covers? They are made again when played.",
               cache_clear_yes);
}

/* ---- the categories ------------------------------------------------------------- */

static const setting_t display_rows[] = {
    { .name = "Brightness", .icon = &icon_sun, .kind = S_PICK, .options = brightness_names, .n = 10, .get = get_brightness, .set = set_brightness },
    { .name = "Screen off", .icon = &icon_screen, .kind = S_PICK, .options = timeout_names, .n = 7, .get = get_timeout, .set = set_timeout },
    { .name = "Keypad light", .icon = &icon_sun, .kind = S_SWITCH, .get = get_keylight, .set = set_keylight },
};
static const setting_t sound_rows[] = {
    { .name = "Music", .icon = &icon_music, .kind = S_PICK, .options = volume_names, .n = 11, .get = get_vol_music, .set = set_vol_music },
    { .name = "Other apps", .icon = &icon_apps, .kind = S_PICK, .options = volume_names, .n = 11, .get = get_vol_app, .set = set_vol_app },
    { .name = "Alarm", .icon = &icon_alarm, .kind = S_PICK, .options = volume_names, .n = 11, .get = get_vol_alarm, .set = set_vol_alarm },
    { .name = "Calls", .icon = &icon_sim, .kind = S_PICK, .options = volume_names, .n = 11, .get = get_vol_call, .set = set_vol_call },
    { .name = "Tactile", .icon = &icon_vibrate, .kind = S_PICK, .options = tactile_names, .n = 4, .get = get_tactile, .set = set_tactile },
    { .name = "Each app", .icon = &icon_speaker, .kind = S_ACTION, .act = open_app_volumes },
};
static const setting_t power_rows[] = {
    { .name = "CPU speed", .icon = &icon_cpu, .kind = S_PICK, .options = cpu_names, .n = 2, .get = get_cpu, .set = set_cpu },
    { .name = "Deep idle", .icon = &icon_power, .kind = S_SWITCH, .get = get_deep_idle, .set = set_deep_idle },
};
static const setting_t storage_rows[] = {
    { .name = "Card", .icon = &icon_card, .kind = S_ACTION, .act = card_info, .detail = card_detail },
    { .name = "Covers", .icon = &icon_music, .kind = S_ACTION, .act = cache_clear, .detail = cache_detail },
};

static const char *const usb_names[] = { "Charge only", "Storage (disk)", "Network (shell)" };
static int get_usb(void) { return ui_settings.usb_default; }
static void set_usb(int v) { ui_settings.usb_default = v; }

static const setting_t usb_rows[] = {
    { .name = "Mode", .icon = &icon_usb, .kind = S_PICK, .options = usb_names, .n = 3,
      .get = get_usb, .set = set_usb },
};

/* ---- date and time ------------------------------------------------------------ */

static const int16_t zones[] = {        /* minutes from UTC, the zones in use */
    -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180, -120, -60,
    0, 60, 120, 180, 210, 240, 270, 300, 330, 345, 360, 390, 420, 480, 525, 540, 570,
    600, 630, 660, 720, 765, 780, 840,
};
#define N_ZONES ((int)(sizeof(zones) / sizeof(zones[0])))
static char zone_text[N_ZONES][10];
static const char *zone_names[N_ZONES];

static void zone_fill(void)
{
    for (int i = 0; i < N_ZONES; i++) {
        int m = zones[i] < 0 ? -zones[i] : zones[i];

        snprintf(zone_text[i], sizeof(zone_text[i]), "UTC%c%d:%02d", zones[i] < 0 ? '-' : '+',
                 m / 60, m % 60);
        zone_names[i] = zone_text[i];
    }
}

static int get_zone(void)
{
    zone_fill();
    for (int i = 0; i < N_ZONES; i++)
        if (zones[i] == ui_settings.tz_min)
            return i;
    return 14;                          /* UTC */
}

/* the clock moves with the zone: the same moment, shown in the new zone */
static void set_zone(int i)
{
    uint32_t now = rtc_unix(ui_settings.tz_min);

    ui_settings.tz_min = zones[i];
    rtc_set_unix(now, ui_settings.tz_min);
    rtc_save();
}

static int get_net_time(void) { return ui_settings.net_time; }
static void set_net_time(int v) { ui_settings.net_time = v; }

static void set_clock(void)
{
    ui_push(app_clock());
}

static void clock_detail(char *out, int n)
{
    rtc_datetime_t t;

    rtc_get(&t);
    snprintf(out, (size_t)n, "%02d:%02d", t.hour, t.min);
}

static const setting_t time_rows[] = {
    { .name = "Set the clock", .icon = &icon_clock, .kind = S_ACTION, .act = set_clock, .detail = clock_detail },
    { .name = "Time zone", .icon = &icon_clock, .kind = S_PICK, .options = zone_names, .n = N_ZONES,
      .get = get_zone, .set = set_zone },
    { .name = "From network", .icon = &icon_usb, .kind = S_SWITCH, .get = get_net_time, .set = set_net_time },
};

static void about(void)
{
    ui_message("About", "B310E OS 0.3\n\nbuilt " BUILD_DATE "\n\nGPLv2 or later\nFatFs (C) ChaN\n"
               "codecs: Rockbox");
}

static const category_t categories[] = {
    { .title = "Display", .icon = &icon_screen, .rows = display_rows, .n = 3 },
    { .title = "Sound & touch", .icon = &icon_speaker, .rows = sound_rows, .n = 6 },
    { .title = "Power", .icon = &icon_power, .rows = power_rows, .n = 2 },
    { .title = "Date & time", .icon = &icon_clock, .rows = time_rows, .n = 3 },
    { .title = "USB", .icon = &icon_usb, .rows = usb_rows, .n = 1 },
    { .title = "Storage", .icon = &icon_card, .rows = storage_rows, .n = 2 },
    { .title = "About", .icon = &icon_info, .act = about },
};

/* ---- a category's page ------------------------------------------------------------ */

static const category_t *page_cat;
static list_t page_list;

static void page_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    const setting_t *r = &page_cat->rows[i];

    snprintf(text, (size_t)tn, "%s", r->name);
    if (r->kind == S_PICK)
        snprintf(detail, (size_t)dn, "%s", r->options[r->get()]);
    else if (r->kind == S_ACTION && r->detail)
        r->detail(detail, dn);
}

static const icon_t *page_icon(void *ctx, int i)
{
    return page_cat->rows[i].icon;
}

static int page_toggle(void *ctx, int i)
{
    const setting_t *r = &page_cat->rows[i];

    return r->kind == S_SWITCH ? r->get() != 0 : -1;
}

static void picked(int v, void *ctx)
{
    const setting_t *r = ctx;

    r->set(v);
    ui_settings_save();
}

static void page_draw(screen_t *s)
{
    list_draw(&page_list);
}

static void page_key(screen_t *s, const key_event_t *ev)
{
    const setting_t *r;

    if (list_key(&page_list, ev))
        return;
    if (ev->code == KEY_RSOFT) {
        ui_pop();
        return;
    }
    if (ev->code != KEY_CENTER && ev->code != KEY_LSOFT)
        return;
    r = &page_cat->rows[page_list.sel];
    switch (r->kind) {
    case S_PICK:
        ui_choose(r->name, r->options, r->n, r->get(), picked, (void *)r);
        break;
    case S_SWITCH:
        r->set(!r->get());
        ui_settings_save();
        break;
    case S_ACTION:
        r->act();
        break;
    }
}

static screen_t page = {
    .left = "Change", .center = "OK", .right = "Back",
    .draw = page_draw, .key = page_key,
};

static void open_category(const category_t *c)
{
    if (c->open) {
        ui_push(c->open());
        return;
    }
    if (c->act) {
        c->act();
        return;
    }
    page_cat = c;
    page.title = c->title;
    page_list.count = c->n;
    page_list.sel = page_list.top = 0;
    page_list.item = page_item;
    page_list.icon = page_icon;
    page_list.toggle = page_toggle;
    ui_push(&page);
}

/* ---- the categories' list -------------------------------------------------------- */

static list_t cat_list;

static void cat_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    snprintf(text, (size_t)tn, "%s", categories[i].title);
}

static const icon_t *cat_icon(void *ctx, int i)
{
    return categories[i].icon;
}

static void cat_draw(screen_t *s)
{
    list_draw(&cat_list);
}

static void cat_key(screen_t *s, const key_event_t *ev)
{
    if (list_key(&cat_list, ev))
        return;
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT)
        open_category(&categories[cat_list.sel]);
    else if (ev->code == KEY_RSOFT)
        ui_pop();
}

static screen_t settings = {
    .title = "Settings", .left = "Open", .center = "OK", .right = "Back",
    .draw = cat_draw, .key = cat_key,
};

screen_t *app_settings(void)
{
    cat_list.count = ARRAY_SIZE(categories);
    cat_list.item = cat_item;
    cat_list.icon = cat_icon;
    return &settings;
}

/* ---- power ------------------------------------------------------------------ */

static const char *const power_names[] = { "Power off", "Restart to menu", "Restart B310E OS" };

static void power_item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    snprintf(text, (size_t)tn, "%s", power_names[i]);
}

static const icon_t *power_icon(void *ctx, int i)
{
    return i == 0 ? &icon_power : &icon_restart;
}

static list_t power_list = { .count = 3, .item = power_item, .icon = power_icon };


/* The boot loader in NOR (boot/) shows its menu after a reset with this
 * request; the reset clears the clock, which clock.txt and SNTP bring
 * back. */
static void to_menu(void)
{
    if (!arch_boot_loader()) {
        ui_message("Restart", "The boot loader is not in NOR.");
        return;
    }
    arch_boot_request(BOOT_REQ_MENU, NULL);
    ui_reboot();
}

/* No reset (the clock stays); the loader's request when that fails. */
static void to_os(void)
{
    const char *err = ui_chain(OS_IMAGE_PATH, "Restarting");

    if (arch_boot_loader()) {
        arch_boot_request(BOOT_REQ_OS, NULL);
        ui_reboot();
    }
    ui_message("Restart", err);
}

static void power_draw(screen_t *s)
{
    list_draw(&power_list);
}

static void power_key(screen_t *s, const key_event_t *ev)
{
    if (list_key(&power_list, ev))
        return;
    if (ev->code == KEY_CENTER || ev->code == KEY_LSOFT) {
        if (power_list.sel == 0)
            ui_confirm("Power off", "Power off the phone?", ui_power_off);
        else if (power_list.sel == 1)
            ui_confirm("Restart", "Start the boot menu?", to_menu);
        else
            ui_confirm("Restart", "Restart B310E OS?", to_os);
    } else if (ev->code == KEY_RSOFT) {
        ui_pop();
    }
}

static screen_t power = {
    .title = "Power", .left = "Select", .center = "OK", .right = "Back",
    .draw = power_draw, .key = power_key,
};

screen_t *app_power(void)
{
    return &power;
}
