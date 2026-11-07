/* B310E OS — the alarm service: the alarms, kept in /b310e-os/alarms.txt
 * (one line each: on, minute, days, label), checked every second by the
 * UI, and the ringing screen.
 *
 * When an alarm's minute comes (on one of its days, or once) the screen
 * wakes and the ringing screen beeps (a 2 kHz tone at the alarm volume,
 * 0.4 s on, 0.4 s off) and buzzes. Snooze (LSoft) rings again in 5
 * minutes; Stop (Center or RSoft) or END ends it; it stops by itself after
 * 3 minutes. A one-time alarm turns itself off once it has rung.
 *
 * It rings only while the OS runs (the screen dark is fine). Powering up
 * the phone for it would take the RTC's alarm registers (+0x20..) and the
 * power-on path, which are not worked out yet.
 *
 * Until 2026-11-07 there was one alarm, in the settings (alarm_on,
 * alarm_min); a card with no alarms file takes that one over.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/alarm.h"
#include "os/audio.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/rtc.h"
#include "os/ui.h"
#include "apps/wavio.h"

#define ALARMS_FILE  OS_DIR "/alarms.txt"
#define SNOOZE_MIN   5
#define RING_MS      (3 * 60 * 1000)
#define BEEP_MS      400
#define BEEP_HZ      2000

static alarm_t alarms[ALARM_MAX];
static int count;
static bool loaded;

int alarm_snooze_min = -1;              /* minute of day to ring again, -1 none (tests read it) */
static int fired_min = -1;              /* the minute that rang (once each) */
bool alarm_ringing;                     /* (tests read it) */
static int ringing_index = -1;

/* ---- the file -------------------------------------------------------------- */

static void save(void)
{
    char line[48];
    FIL f;
    UINT n;

    if (f_open(&f, ALARMS_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return;
    for (int i = 0; i < count; i++) {
        int len = snprintf(line, sizeof(line), "%u %u %u %s\n", alarms[i].on,
                           alarms[i].minute, alarms[i].days, alarms[i].label);

        f_write(&f, line, (UINT)len, &n);
    }
    f_close(&f);
}

static int number(const char **p)
{
    int v = 0;

    while (**p == ' ')
        (*p)++;
    while (**p >= '0' && **p <= '9')
        v = v * 10 + *(*p)++ - '0';
    return v;
}

static void load(void)
{
    char buf[ALARM_MAX * 48 + 1], *p;
    FIL f;
    UINT n = 0;

    loaded = true;
    count = 0;
    if (f_open(&f, ALARMS_FILE, FA_READ) != FR_OK) {
        /* the single alarm of before, from the settings */
        if (ui_settings.alarm_on || ui_settings.alarm_min != 7 * 60) {
            alarms[0].on = (uint8_t)ui_settings.alarm_on;
            alarms[0].minute = (uint16_t)ui_settings.alarm_min;
            alarms[0].days = ALARM_ONCE;
            alarms[0].label[0] = 0;
            count = 1;
            save();
        }
        return;
    }
    f_read(&f, buf, sizeof(buf) - 1, &n);
    f_close(&f);
    buf[n] = 0;
    for (p = buf; *p && count < ALARM_MAX; ) {
        const char *q = p;
        alarm_t *a = &alarms[count];
        int on, minute, days, k = 0;

        while (*p && *p != '\n')
            p++;
        if (*p)
            *p++ = 0;
        on = number(&q);
        minute = number(&q);
        days = number(&q);
        if (*q == ' ')
            q++;
        while (*q && *q != '\r' && k < (int)sizeof(a->label) - 1)
            a->label[k++] = *q++;
        a->label[k] = 0;
        if (minute >= 24 * 60 || days > 127)
            continue;
        a->on = on != 0;
        a->minute = (uint16_t)minute;
        a->days = (uint8_t)days;
        count++;
    }
}

static void ensure(void)
{
    if (!loaded)
        load();
}

/* ---- the interface ---------------------------------------------------------------- */

int alarm_count(void)
{
    ensure();
    return count;
}

bool alarm_get(int i, alarm_t *out)
{
    ensure();
    if (i < 0 || i >= count)
        return false;
    *out = alarms[i];
    return true;
}

int alarm_set(int i, const alarm_t *a)
{
    ensure();
    if (i < 0 || i > count || i >= ALARM_MAX || a->minute >= 24 * 60)
        return -1;
    alarms[i] = *a;
    alarms[i].label[sizeof(alarms[i].label) - 1] = 0;
    alarms[i].days &= 0x7f;
    if (i == count)
        count++;
    alarm_snooze_min = -1;
    save();
    return i;
}

bool alarm_delete(int i)
{
    ensure();
    if (i < 0 || i >= count)
        return false;
    memmove(&alarms[i], &alarms[i + 1], sizeof(alarm_t) * (size_t)(count - i - 1));
    count--;
    save();
    return true;
}

void alarm_days_text(uint8_t days, char *out, int n)
{
    static const char *const names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    int len = 0;

    days &= 0x7f;
    if (!days) {
        snprintf(out, (size_t)n, "Once");
        return;
    }
    if (days == 0x7f) {
        snprintf(out, (size_t)n, "Every day");
        return;
    }
    if (days == 0x3e) {
        snprintf(out, (size_t)n, "Weekdays");
        return;
    }
    if (days == 0x41) {
        snprintf(out, (size_t)n, "Weekends");
        return;
    }
    out[0] = 0;
    for (int d = 1; d <= 7; d++) {      /* Monday first */
        int w = d % 7;

        if (days & 1u << w)
            len += snprintf(out + len, (size_t)(n - len), "%s%s", len ? " " : "", names[w]);
        if (len >= n - 1)
            break;
    }
}

int alarm_next(uint32_t *minutes_until)
{
    rtc_datetime_t t;
    int best = -1;
    uint32_t best_in = 0xffffffffu;

    ensure();
    if (rtc_get(&t) != 0)
        return -1;
    for (int i = 0; i < count; i++) {
        int now = t.hour * 60 + t.min;

        if (!alarms[i].on)
            continue;
        for (int d = 0; d < 8; d++) {   /* today .. a week on */
            int wday = (t.wday + d) % 7;
            int32_t in = d * 24 * 60 + alarms[i].minute - now;

            if (in <= 0 && d == 0)
                continue;
            if (alarms[i].days && !(alarms[i].days & 1u << wday))
                continue;
            if ((uint32_t)in < best_in) {
                best_in = (uint32_t)in;
                best = i;
            }
            break;
        }
    }
    if (minutes_until)
        *minutes_until = best_in;
    return best;
}

/* ---- the ringing screen ------------------------------------------------------------ */

static uint32_t ring_start;
static int ring_min;                    /* the minute it rang at */
static bool beep_on;

static void ring_stop(void)
{
    if (beep_on) {
        wav_stop();
        vibrator_set(0);
        beep_on = false;
    }
}

static void ring_tick(screen_t *s)
{
    uint32_t t = kernel_ticks() - ring_start;
    bool want = (t / BEEP_MS) % 2 == 0;

    if (t >= RING_MS) {
        ui_pop();                       /* leave() stops it */
        return;
    }
    if (want == beep_on)
        return;
    beep_on = want;
    if (want) {
        volume_use(VOL_ALARM);
        wav_play_tone(BEEP_HZ);
        vibrator_set(5);
    } else {
        wav_stop();
        vibrator_set(0);
    }
}

static void ring_draw(screen_t *s)
{
    char buf[24];

    snprintf(buf, sizeof(buf), "%02d:%02d", ring_min / 60, ring_min % 60);
    gfx_text_centered(UI_TOP + 24, buf, C_ACCENT, FONT_HUGE, 1);
    if (ringing_index >= 0 && ringing_index < count && alarms[ringing_index].label[0])
        snprintf(buf, sizeof(buf), "%s", alarms[ringing_index].label);
    else
        snprintf(buf, sizeof(buf), "Alarm");
    {                                   /* centred, cut to the screen */
        int w = gfx_text_width(buf, FONT_LARGE, 1), max = LCD_WIDTH - 2 * UI_PAD;

        gfx_text((LCD_WIDTH - (w < max ? w : max)) / 2, UI_TOP + 62, buf, C_TEXT, FONT_LARGE,
                 1, max);
    }
    gfx_text_centered(UI_TOP + 80, "Snooze: 5 minutes", C_DIM, FONT_SMALL, 1);
}

static void ring_key(screen_t *s, const key_event_t *ev)
{
    if (ev->code == KEY_LSOFT) {
        int h, m, sec;

        rtc_time(&h, &m, &sec);
        alarm_snooze_min = (h * 60 + m + SNOOZE_MIN) % (24 * 60);
        ui_pop();
    } else if (ev->code == KEY_CENTER || ev->code == KEY_RSOFT) {
        alarm_snooze_min = -1;
        ui_pop();
    }
}

static void ring_leave(screen_t *s)
{
    ring_stop();
    alarm_ringing = false;
}

static screen_t ring_screen = {
    .title = "Alarm", .left = "Snooze", .center = "Stop", .right = "Stop",
    .draw = ring_draw, .key = ring_key, .tick = ring_tick, .leave = ring_leave,
    .tick_ms = 100,
};

static void ring(int index, int minute)
{
    fired_min = minute;
    ringing_index = index;
    alarm_ringing = true;
    ring_min = minute;
    ring_start = kernel_ticks();
    beep_on = false;
    klog("alarm: ringing at %02d:%02d", minute / 60, minute % 60);
    ui_wake();
    ui_push(&ring_screen);
}

void alarm_check(int h, int m)
{
    int now = h * 60 + m;
    rtc_datetime_t t;
    int wday = -1;

    ensure();
    if (now != fired_min)
        fired_min = -1;
    if (alarm_ringing || fired_min == now)
        return;
    if (now == alarm_snooze_min) {
        alarm_snooze_min = -1;
        ring(ringing_index, now);
        return;
    }
    for (int i = 0; i < count; i++) {
        if (!alarms[i].on || alarms[i].minute != now)
            continue;
        if (alarms[i].days) {
            if (wday < 0)
                wday = rtc_get(&t) == 0 ? t.wday : -1;
            if (wday < 0 || !(alarms[i].days & 1u << wday))
                continue;
        } else {
            alarms[i].on = 0;           /* once: done */
            save();
        }
        ring(i, now);
        return;
    }
}
