/* B310E OS — the alarm service: the alarms, kept in the store's "alarms"
 * table (os/db.h: on, minute, days, label; in the order they were added),
 * checked every second by the UI, and the ringing screen.
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
 * alarm_min); a card with no alarms file takes that one over. Until
 * 2026-11-17 they were lines of /b310e-os/alarms.txt (on, minute, days,
 * label), read once into the store.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/alarm.h"
#include "os/audio.h"
#include "os/db.h"
#include "os/drivers.h"
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/media.h"
#include "os/rtc.h"
#include "os/ui.h"
#include "apps/wavio.h"

#define OLD_FILE     OS_DIR "/alarms.txt"
#define SNOOZE_MIN   5
#define RING_MS      (3 * 60 * 1000)
#define BEEP_MS      400
#define BEEP_HZ      2000

static alarm_t alarms[ALARM_MAX];
static int count;
static bool loaded;

int alarm_snooze_min = -1;              /* minute of day to ring again, -1 none (tests read it) */
bool alarm_ringing;                     /* (tests read it) */
static int ringing_index = -1;
/* A snooze is a time span in ticks, so a clock moved meanwhile cannot make it 24 h;
 * and one each, or stopping another alarm would cancel it */
static uint32_t snooze_at[ALARM_MAX];
static bool snoozed[ALARM_MAX];
static unsigned pending;                /* due while another rang: bit per alarm */
static int pending_min = -1;
/* what the ring took over, given back when it ends */
static bool took_music, took_fm, took_volume;
static int took_cat;

/* ---- the store ---------------------------------------------------------------- */

enum { F_ON, F_MINUTE, F_DAYS, F_LABEL };
static const db_field_t fields[] = {
    { "on", DB_BOOL }, { "minute", DB_INT }, { "days", DB_INT }, { "label", DB_STR },
};
static const db_schema_t schema = { "alarms", 1, 4, -1, fields, NULL };
static db_table_t *table;
static uint32_t ids[ALARM_MAX];         /* each alarm's record, 0 none yet */
static uint32_t own_gen;                /* the table as we last left it */

static void save(int i)
{
    db_value_t v[4] = {
        DB_INTV(alarms[i].on), DB_INTV(alarms[i].minute), DB_INTV(alarms[i].days),
        DB_STRV(alarms[i].label),
    };

    if (!table)
        table = db_table_open(&schema);
    if (!table)
        return;
    if (!ids[i] || db_update(table, ids[i], v, 4) == DB_ENOENT)
        ids[i] = db_insert(table, v, 4);
    own_gen = db_table_gen(table);
}

static void unsave(int i)
{
    if (table && ids[i])
        db_remove(table, ids[i]);
    memmove(&ids[i], &ids[i + 1], sizeof(ids[0]) * (size_t)(ALARM_MAX - i - 1));
    ids[ALARM_MAX - 1] = 0;
    if (table)
        own_gen = db_table_gen(table);
}

/* The table's records, oldest first: the alarms in the order they were added */
static void from_table(void)
{
    uint32_t found[ALARM_MAX];
    int n = db_find(table, -1, NULL, found, ALARM_MAX, 0);

    count = 0;
    while (n-- > 0) {
        char buf[96];
        db_value_t v[4];
        alarm_t *a = &alarms[count];
        int r = db_get(table, found[n], v, 4, buf, sizeof(buf));
        size_t len;

        if (r == DB_ETOOBIG) {          /* a label from elsewhere, too long: none */
            v[F_LABEL].p = "";
            v[F_LABEL].len = 0;
        } else if (r) {
            continue;
        }
        if (v[F_MINUTE].i < 0 || v[F_MINUTE].i >= 24 * 60 || v[F_DAYS].i < 0 || v[F_DAYS].i > 127)
            continue;
        a->on = v[F_ON].i != 0;
        a->minute = (uint16_t)v[F_MINUTE].i;
        a->days = (uint8_t)v[F_DAYS].i;
        len = v[F_LABEL].len < sizeof(a->label) ? v[F_LABEL].len :
              utf8_cut(v[F_LABEL].p, sizeof(a->label) - 1);
        memcpy(a->label, v[F_LABEL].p, len);
        a->label[len] = 0;
        ids[count++] = found[n];
    }
    for (int i = count; i < ALARM_MAX; i++)
        ids[i] = 0;
    own_gen = db_table_gen(table);
}

/* Changed by someone else (an import, the shell): read again at the next look */
static void table_changed(void *ctx)
{
    if (table && db_table_gen(table) != own_gen)
        loaded = false;
}

/* ---- the old file, read once ------------------------------------------------------ */

static int number(const char **p)
{
    int v = 0;

    while (**p == ' ')
        (*p)++;
    while (**p >= '0' && **p <= '9')
        v = v * 10 + *(*p)++ - '0';
    return v;
}

static void import_file(void)
{
    char buf[ALARM_MAX * 48 + 1], *p;
    FIL f;
    UINT n = 0;

    count = 0;
    if (f_open(&f, OLD_FILE, FA_READ) != FR_OK) {
        /* the single alarm of before, from the settings */
        if (ui_settings.alarm_on || ui_settings.alarm_min != 7 * 60) {
            alarms[0].on = (uint8_t)ui_settings.alarm_on;
            alarms[0].minute = (uint16_t)ui_settings.alarm_min;
            alarms[0].days = ALARM_ONCE;
            alarms[0].label[0] = 0;
            count = 1;
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
        while (q[k] && q[k] != '\r')
            k++;
        /* UTF-8 now: a byte that is not (a PC's Latin-1) taken as one, never cut mid-way */
        utf8_from_latin1(q, (size_t)k, a->label, sizeof(a->label));
        if (minute >= 24 * 60 || days > 127)
            continue;
        a->on = on != 0;
        a->minute = (uint16_t)minute;
        a->days = (uint8_t)days;
        count++;
    }
    klog("alarm: %s read into the store, %d alarms", OLD_FILE, count);
}

static void load(void)
{
    static bool watching;

    loaded = true;
    if (!watching)
        watching = db_watch("alarms", true, table_changed, NULL);
    if (!table)
        table = db_table_open(&schema);
    if (!db_get_bool("import.alarms", false)) {
        import_file();
        for (int i = 0; i < ALARM_MAX; i++)
            ids[i] = 0;
        for (int i = 0; i < count; i++)
            save(i);
        db_set_bool("import.alarms", true);
        return;
    }
    count = 0;
    if (table)
        from_table();
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
    {                                   /* UTF-8 the store takes: whole characters */
        char *l = alarms[i].label, t[sizeof(alarms[i].label)];

        l[utf8_cut(l, strlen(l))] = 0;
        if (!utf8_valid(l, strlen(l))) {
            utf8_from_latin1(l, strlen(l), t, sizeof(t));
            memcpy(l, t, sizeof(t));
        }
    }
    alarms[i].days &= 0x7f;
    if (i == count)
        count++;
    snoozed[i] = false;                 /* changed: its snooze no longer means it */
    pending &= ~(1u << i);
    save(i);
    return i;
}

bool alarm_delete(int i)
{
    ensure();
    if (i < 0 || i >= count)
        return false;
    if (i < ringing_index)
        ringing_index--;
    else if (i == ringing_index)
        ringing_index = -1;             /* the one ringing: none, not the next one */
    for (int j = i; j + 1 < count; j++) {
        snoozed[j] = snoozed[j + 1];
        snooze_at[j] = snooze_at[j + 1];
    }
    snoozed[count - 1] = false;
    pending = (pending & ((1u << i) - 1)) | (pending >> (i + 1) << i);
    memmove(&alarms[i], &alarms[i + 1], sizeof(alarm_t) * (size_t)(count - i - 1));
    count--;
    unsave(i);
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
        if (!wav_recording())
            wav_stop();
        vibrator_set(0);
        beep_on = false;
    }
}

static uint32_t ring_ticked;            /* its last tick: none for long = covered */

static void ring_tick(screen_t *s)
{
    uint32_t t = kernel_ticks() - ring_start;
    bool want = (t / BEEP_MS) % 2 == 0;

    ring_ticked = kernel_ticks();
    if (t >= RING_MS) {
        ui_pop();                       /* leave() stops it */
        return;
    }
    if (want == beep_on)
        return;
    beep_on = want;
    if (want) {
        if (!wav_recording()) {         /* a tone would end the recording: buzz only */
            volume_use(VOL_ALARM);
            wav_play_tone(BEEP_HZ);
        }
        vibrator_set(5);
    } else {
        if (!wav_recording())
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
        if (ringing_index >= 0 && ringing_index < ALARM_MAX) {
            snoozed[ringing_index] = true;
            snooze_at[ringing_index] = kernel_ticks() + SNOOZE_MIN * 60000u;
        }
        ui_pop();
    } else if (ev->code == KEY_CENTER || ev->code == KEY_RSOFT) {
        alarm_snooze_min = -1;
        if (ringing_index >= 0 && ringing_index < ALARM_MAX)
            snoozed[ringing_index] = false;
        ui_pop();
    }
}

static void ring_leave(screen_t *s)
{
    ring_stop();
    if (took_fm && fm_is_on())          /* it may have been turned off meanwhile */
        audio_fm_route(true);
    if (took_volume)
        volume_use(took_cat);
    if (took_music)
        media_pause(false);
    took_fm = took_music = took_volume = false;
    alarm_ringing = false;
}

static screen_t ring_screen = {
    .title = "Alarm", .left = "Snooze", .center = "Stop", .right = "Stop",
    .draw = ring_draw, .key = ring_key, .tick = ring_tick, .leave = ring_leave,
    .tick_ms = 100,
};

static void ring(int index, int minute)
{
    ringing_index = index;
    alarm_ringing = true;
    ring_min = minute;
    ring_start = ring_ticked = kernel_ticks();
    beep_on = false;
    klog("alarm: ringing at %02d:%02d", minute / 60, minute % 60);
    /* Its tone needs the DAC and its own volume: the music waits, and the radio's
     * line-in route (which leaves the DAC out, so the tone would not be heard) goes
     * until it ends. A recording stays, with the buzz only. */
    if (!wav_recording()) {
        took_cat = volume_current();
        took_volume = true;
        if (media_state() == MEDIA_PLAYING || media_state() == MEDIA_LOADING) {
            media_pause(true);
            took_music = true;
        }
        if (fm_is_on()) {
            audio_fm_route(false);
            took_fm = true;
        }
    }
    ui_wake();
    ui_push_urgent(&ring_screen);
    ui_screen_mode(&ring_screen, UI_MODE_AWAKE);    /* lit for all of its 3 minutes */
}

static bool due(int i, int now, int *wday)
{
    rtc_datetime_t t;

    if (!alarms[i].on || alarms[i].minute != now)
        return false;
    if (!alarms[i].days)
        return true;
    if (*wday < 0)
        *wday = rtc_get(&t) == 0 ? t.wday : -1;
    return *wday >= 0 && (alarms[i].days & 1u << *wday);
}

static void ring_alarm(int i, int now)
{
    if (!alarms[i].days) {
        alarms[i].on = 0;               /* once: done */
        save(i);
    }
    ring(i, now);
}

void alarm_check(int h, int m)
{
    int now = h * 60 + m, wday = -1;

    ensure();
    /* Each new minute once: what is due joins the queue, also during a ring, so that
     * nothing is lost to a ring or a snooze landing on the same minute */
    if (now != pending_min) {
        pending_min = now;
        for (int i = 0; i < count; i++)
            if (due(i, now, &wday))
                pending |= 1u << i;
    }
    /* A screen over the ring (a message) stops its tick: the buzz goes off rather
     * than stay on, and once uncovered past RING_MS its tick closes it at once */
    if (alarm_ringing && beep_on && kernel_ticks() - ring_ticked > 1000)
        ring_stop();
    if (alarm_ringing)
        return;
    for (int i = 0; i < count; i++)
        if (snoozed[i] && (int32_t)(kernel_ticks() - snooze_at[i]) >= 0) {
            snoozed[i] = false;
            alarm_snooze_min = -1;
            ring(i, now);
            return;
        }
    for (int i = 0; i < count; i++)
        if (pending & 1u << i) {
            pending &= ~(1u << i);
            ring_alarm(i, now);
            return;
        }
    pending = 0;
}
