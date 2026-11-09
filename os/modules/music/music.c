/* Music — the play queue's face (os/media.h): a list of the queued tracks
 * and Now playing. The queue lives in the OS and plays on in the
 * background when the app closes; the app fills it and shows it.
 *
 * Opened with (app_arg()):
 *   nothing      Now playing when the queue plays, else the list; an empty
 *                queue is filled from /music and its folders (3 deep), or
 *                else the recordings in /b310e-os/rec
 *   an .m3u      the queue becomes the list, played from its first track
 *   a song       the queue becomes the song's folder, played from the song
 * (Files opens Music with the path of a song or .m3u.) Playable: every
 * extension a codec module on the card claims.
 *
 * The list: Up/Down move, Center plays the selected track, LSoft turns
 * shuffle on and off. Now playing: the cover (60 x 60, or a note while it
 * decodes or when there is none), title, artist, a position bar with the
 * elapsed and total time; Center pauses and resumes, Left/Right go to the
 * previous and next track (previous restarts the track after its first
 * 3 s), held they seek (faster the longer they are held; the new position
 * is shown while held and played when let go), Up/Down set the music's
 * volume (a bar over the cover's foot), * shuffle, # repeat (all, one,
 * off), LSoft the list. RSoft leaves; the music plays on.
 *
 * Until 2026-11-07 this was the built-in apps/music.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "icons.h"

#define PATH_MAX_   160
#define SCAN_DEPTH  3                   /* folders under /music */
#define REC_DIR     "/b310e-os/rec"     /* the recordings (apps/wavio.h) */
#define TAP_MS      600                 /* Left/Right let go before a repeat */
#define REPEAT_GAP_MS 400               /* the UI's first repeat comes at 450 ms */
#define HOLD_END_MS 260                 /* repeats stopped: the key is up */
#define PREV_MS     3000                /* previous restarts a track after this */
#define VOLUME_MS   1500                /* the volume bar stays */
#define ART_SIDE    60

enum { S_LIST, S_NP };

static list_t list;
static const icon_t *ic_play, *ic_music;
static int seen_cur = -2;               /* the list follows the track playing */

/* the app's screens on the stack, bottom first (pushed and popped in turn) */
static uint8_t shown[8];
static int n_shown;

/* Left/Right: a tap skips, a hold seeks */
static int tap_dir, seek_dir;
static uint32_t tap_at, last_repeat, volume_at;
static int32_t seek_pos = -1;
static int seek_steps;

static const screen_t list_tpl, np_tpl;

/* a copy of a screen, freed when popped: the app may be opened again
 * (app_launch) while one of its screens is on the stack */
static void note_ensure(void);          /* the notification, below */
static void quit(void);

/* Open, or paused in the background, and playing nothing for player_idle_s with
 * no key: it closes, since only a reboot would otherwise end a paused queue. */
static uint32_t used_at;
static bool idle_said;
static int seek_cur = -1;               /* the track a held key is seeking in */

static void used(void)
{
    used_at = ticks();
    idle_said = false;
}

static bool idle_long(void)
{
    if (media_queue_active() && media_state() != MEDIA_PAUSED)
        used();
    if (!ui_settings.player_idle_s ||
        ticks() - used_at < (uint32_t)ui_settings.player_idle_s * 1000u)
        return false;
    if (!idle_said)
        klog("music: nothing played for %d s: closing", ui_settings.player_idle_s);
    idle_said = true;                   /* still true: a screen under the panel closes
                                         * as soon as it is back on top */
    return true;
}

static screen_t *new_screen(const screen_t *tpl)
{
    screen_t *s = malloc(sizeof(*s));

    if (s) {
        memcpy(s, tpl, sizeof(*s));
        s->free_on_pop = true;
    }
    return s;
}

static void shown_push(int kind)
{
    if (n_shown < (int)sizeof(shown))
        shown[n_shown] = (uint8_t)kind;
    n_shown++;
}

static void shown_pop(void)
{
    if (n_shown)
        n_shown--;
}

/* the app's screen under the top one is of this kind */
static bool under_is(int kind)
{
    return n_shown >= 2 && n_shown <= (int)sizeof(shown) && shown[n_shown - 2] == kind;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');

    return s ? s + 1 : path;
}

static bool is_m3u(const char *name)
{
    const char *dot = strrchr(name, '.');

    return dot && (!strcmp(dot, ".m3u") || !strcmp(dot, ".M3U") ||
                   !strcmp(dot, ".m3u8") || !strcmp(dot, ".M3U8"));
}

static void time_text(char *buf, size_t n, uint32_t ms)
{
    uint32_t s = ms / 1000;

    if (s >= 3600)
        snprintf(buf, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600),
                 (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else
        snprintf(buf, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static bool paused(void)
{
    return media_queue_active() && media_state() == MEDIA_PAUSED;
}

static void play(int i)
{
    media_queue_play(i);
    seek_pos = -1;
}

static void toggle_pause(void)
{
    media_queue_toggle();
    seek_pos = -1;
}

static void change_volume(int up)
{
    int v = volume_level(VOL_MUSIC) + (up ? 1 : -1);

    if (v < 0)
        v = 0;
    if (v > VOL_LEVELS)
        v = VOL_LEVELS;
    volume_set(VOL_MUSIC, v);
    volume_at = ticks();
}

static bool volume_shown(void)
{
    return volume_at && ticks() - volume_at < VOLUME_MS;
}

static const char *repeat_name(void)
{
    int r = media_queue_repeat();

    return r == MEDIA_REPEAT_ONE ? "Repeat one" : r == MEDIA_REPEAT_OFF ? "No repeat" : "Repeat all";
}

/* text centred in the screen's width, cut with ".." when wider */
static void centered(int y, const char *s, uint16_t c, int font)
{
    int max = LCD_WIDTH - 2 * UI_PAD, w = gfx_text_width(s, font, 1);

    if (w > max)
        w = max;
    gfx_text((LCD_WIDTH - w) / 2, y, s, c, font, 1, max);
}

/* ---- now playing ----------------------------------------------------------- */

static void skip(int d)
{
    if (d < 0 && media_queue_active() && media_position() > PREV_MS)
        media_seek(0);                  /* back to the start of this one */
    else
        media_queue_step(d);
    seek_pos = -1;
}

/* the bar, the times and the state between them, at y */
static void draw_position(int y, uint32_t pos, uint32_t len, media_state_t st)
{
    char t1[12], t2[12];
    int w = LCD_WIDTH - 2 * UI_PAD;

    gfx_bar(UI_PAD, y, w, 5, len ? (int)(pos / 100) : 0, len ? (int)(len / 100) : 1,
            seek_pos >= 0 ? C_OK : C_ACCENT);
    time_text(t1, sizeof(t1), pos);
    gfx_text(UI_PAD, y + 9, t1, C_TEXT, FONT_SMALL, 1, w / 2);
    if (len) {
        time_text(t2, sizeof(t2), len);
        gfx_text(LCD_WIDTH - UI_PAD - gfx_text_width(t2, FONT_SMALL, 1), y + 9, t2,
                 C_TEXT, FONT_SMALL, 1, w / 2);
    }
    if (st == MEDIA_PAUSED || !media_queue_active()) {
        gfx_fill(LCD_WIDTH / 2 - 4, y + 8, 3, 9, C_TEXT);
        gfx_fill(LCD_WIDTH / 2 + 1, y + 8, 3, 9, C_TEXT);
    } else if (ic_play) {
        gfx_icon(LCD_WIDTH / 2 - ICON_SIZE / 2, y + 7, ic_play,
                 st == MEDIA_LOADING ? C_DIM : C_ACCENT);
    }
}

/* With a cover: it on top, the title and artist under it, then the
 * position; the volume, while it changes, over the cover's foot. */
static void np_draw_art(const media_info_t *mi, const uint16_t *art, const char *name,
                        uint32_t pos, uint32_t len, media_state_t st)
{
    int x = (LCD_WIDTH - ART_SIDE) / 2, y = UI_TOP + 13;
    const char *err = media_queue_error();

    gfx_round(x - 2, y - 2, ART_SIDE + 4, ART_SIDE + 4, UI_RADIUS, C_CARD_HI);
    if (art) {
        gfx_blit(x, y, ART_SIDE, ART_SIDE, art);
    } else {                            /* decoding, or none: a note on a card */
        gfx_fill(x, y, ART_SIDE, ART_SIDE, C_CARD);
        if (ic_music)
            gfx_icon(x + (ART_SIDE - ICON_SIZE) / 2, y + (ART_SIDE - ICON_SIZE) / 2, ic_music,
                     C_DIM);
    }
    if (volume_shown()) {
        gfx_shade(x, y + ART_SIDE - 12, ART_SIDE, 12, C_BG, 200);
        gfx_bar(x + 4, y + ART_SIDE - 8, ART_SIDE - 8, 4, volume_level(VOL_MUSIC), VOL_LEVELS,
                C_ACCENT);
    }
    y += ART_SIDE + 4;
    centered(y, mi->title[0] ? mi->title : name, C_TEXT, FONT_BODY);
    if (err)
        centered(y + 12, err, C_BAD, FONT_SMALL);
    else
        centered(y + 12, mi->artist[0] ? mi->artist : mi->album, C_ACCENT, FONT_SMALL);
    draw_position(UI_BOTTOM - 21, pos, len, st);
}

static void np_draw(screen_t *s)
{
    media_info_t mi;
    char buf[48];
    bool on = media_queue_active();
    bool have = on && media_info(&mi);
    media_state_t st = on ? media_state() : MEDIA_IDLE;
    uint32_t pos = seek_pos >= 0 ? (uint32_t)seek_pos : media_position();
    uint32_t len = have ? mi.length_ms : 0;
    int count = media_queue_count(), cur = media_queue_current();
    const char *path = media_queue_path(cur);
    const char *name = path ? base_name(path) : "";
    const char *err = media_queue_error();
    int y = UI_TOP + 4, w = LCD_WIDTH - 2 * UI_PAD;

    s->center = st == MEDIA_PAUSED || !on ? "Play" : "Pause";
    if (!count) {
        centered(UI_TOP + 40, "Nothing queued", C_TEXT, FONT_LARGE);
        return;
    }
    /* the line on top: n of N, the format */
    snprintf(buf, sizeof(buf), "%d of %d", cur + 1, count);
    gfx_text(UI_PAD, y, buf, C_DIM, FONT_SMALL, 1, w / 2);
    if (have) {
        if (mi.bitrate)
            snprintf(buf, sizeof(buf), "%s %luk", mi.codec, (unsigned long)mi.bitrate);
        else
            snprintf(buf, sizeof(buf), "%s", mi.codec);
        gfx_text(LCD_WIDTH - UI_PAD - gfx_text_width(buf, FONT_SMALL, 1), y, buf, C_DIM,
                 FONT_SMALL, 1, w / 2);
    }

    /* the cover (decoded in the background; a placeholder until then, or
     * when there is none, so the layout does not jump) */
    if (have) {
        art_request(mi.path, &mi, ART_SIDE);
        np_draw_art(&mi, art_get(mi.path, ART_SIDE, NULL), name, pos, len, st);
        return;
    }

    /* the card: the file's name until the tags are known */
    gfx_round(UI_PAD, y + 12, w, 50, UI_RADIUS, C_CARD);
    centered(y + 18, name, C_TEXT, FONT_LARGE);
    centered(y + 34, on ? "..." : "", C_ACCENT, FONT_BODY);

    /* the position */
    y += 70;
    draw_position(y, pos, len, st);

    /* modes, an error, or the volume while it changes */
    y += 24;
    if (volume_shown()) {
        int v = volume_level(VOL_MUSIC);

        if (v <= 0)
            snprintf(buf, sizeof(buf), "Volume: mute");
        else
            snprintf(buf, sizeof(buf), "Volume %d", v);
        gfx_text(UI_PAD, y, buf, C_TEXT, FONT_SMALL, 1, w / 2 + 10);
        gfx_bar(LCD_WIDTH / 2 + 14, y + 1, w / 2 - 14, 5, v, VOL_LEVELS, C_ACCENT);
    } else if (err) {
        centered(y, err, C_BAD, FONT_SMALL);
    } else {
        snprintf(buf, sizeof(buf), "%s  %s", media_queue_shuffle() ? "Shuffle" : "In order",
                 repeat_name());
        centered(y, buf, C_DIM, FONT_SMALL);
    }
}

static void show_list(void)
{
    screen_t *s;

    if (under_is(S_LIST))
        ui_pop();                       /* the list is under this screen */
    else if ((s = new_screen(&list_tpl)))
        ui_push(s);
}

static void np_key(screen_t *s, const key_event_t *ev)
{
    used();
    uint32_t now = ev->time;

    switch (ev->code) {
    case KEY_CENTER:
    case KEY_HS_CENTER:
        toggle_pause();
        break;
    case KEY_LEFT:
    case KEY_RIGHT: {
        int d = ev->code == KEY_RIGHT ? 1 : -1;

        if (seek_dir == d) {            /* a repeat while seeking */
            int32_t len = (int32_t)media_duration();
            int32_t inc = seek_steps < 10 ? 2000 : seek_steps < 30 ? 5000 : 15000;

            seek_steps++;
            seek_pos += d * inc;
            if (seek_pos < 0)
                seek_pos = 0;
            if (len && seek_pos > len - 1000)
                seek_pos = len > 1000 ? len - 1000 : 0;
            last_repeat = now;
        } else if (tap_dir == d && now - tap_at >= REPEAT_GAP_MS && media_queue_active()) {
            seek_dir = d;               /* the first repeat: held, seek */
            seek_steps = 1;
            seek_pos = (int32_t)media_position() + d * 2000;
            seek_cur = media_queue_current();
            if (seek_pos < 0)
                seek_pos = 0;
            tap_dir = 0;
            last_repeat = now;
        } else {
            if (tap_dir)
                skip(tap_dir);          /* a quick second tap */
            tap_dir = d;
            tap_at = now;
        }
        break;
    }
    case KEY_UP:
    case KEY_HS_PLUS:
        change_volume(1);
        break;
    case KEY_DOWN:
    case KEY_HS_MINUS:
        change_volume(0);
        break;
    case KEY_STAR:
        media_queue_set_shuffle(!media_queue_shuffle());
        break;
    case KEY_HASH:
        media_queue_set_repeat((media_queue_repeat() + 1) % 3);
        break;
    case KEY_LSOFT:
        show_list();
        break;
    case KEY_RSOFT:
        ui_pop();                       /* the music plays on */
        break;
    }
}

static void np_tick(screen_t *s)
{
    uint32_t now = ticks();

    if (idle_long()) {
        quit();
        return;
    }

    if (tap_dir && now - tap_at > TAP_MS) {
        int d = tap_dir;

        tap_dir = 0;
        skip(d);
    }
    note_ensure();
    if (seek_dir && now - last_repeat > HOLD_END_MS) {
        seek_dir = 0;
        if (seek_pos >= 0 && seek_cur == media_queue_current()) {  /* not if it moved on */
            klog("music: seek to %lu ms", (unsigned long)seek_pos);
            media_seek((uint32_t)seek_pos);
        }
        seek_pos = -1;
    }
}

/* ---- the notification: the track and its buttons while the queue plays,
 * open or not (it keeps this app loaded; it goes when the music stops) */

static const icon_t app_icon;
static notify_t note;

static bool note_update(notify_t *n)
{
    media_info_t mi;
    const char *now = media_queue_now();
    uint32_t len = media_duration();

    if (idle_long())                    /* a screen of its own closes at its tick */
        media_queue_clear();
    if (!media_queue_active())
        return false;
    if (media_info(&mi) && mi.title[0]) {
        snprintf(n->title, sizeof(n->title), "%s", mi.title);
        snprintf(n->text, sizeof(n->text), "%s", mi.artist);
    } else {
        snprintf(n->title, sizeof(n->title), "%s", now ? now : "Music");
        n->text[0] = 0;
    }
    n->progress = len ? (int)((uint64_t)media_position() * 1000 / len) : 0;
    if (n->progress < 1 && len)
        n->progress = 1;
    n->action[1] = media_state() == MEDIA_PAUSED ? "Play" : "Pause";
    return true;
}

static void note_act(notify_t *n, int a)
{
    used();
    if (a == 1)
        toggle_pause();
    else
        skip(a == 0 ? -1 : 1);
}

static void note_ensure(void)
{
    for (int i = 0; i < notify_count(); i++)
        if (notify_get(i) == &note)
            return;
    if (!media_queue_active())
        return;
    note.icon = &app_icon;
    note.action[0] = "|<";
    note.action[2] = ">|";
    note.act = note_act;
    note.update = note_update;
    note_update(&note);
    notify_post(&note);
}

static void np_enter(screen_t *s)
{
    shown_push(S_NP);
}

static void np_leave(screen_t *s)
{
    note_ensure();                      /* before the app may be unloaded */
    shown_pop();
    tap_dir = seek_dir = 0;
    seek_pos = -1;
}

static const screen_t np_tpl = {
    .title = "Now playing", .left = "List", .center = "Pause", .right = "Back",
    .enter = np_enter, .leave = np_leave, .draw = np_draw, .key = np_key,
    .tick = np_tick, .tick_ms = 100,
};

/* ---- the list --------------------------------------------------------------- */

static void item(void *ctx, int i, char *text, int tn, char *detail, int dn)
{
    const char *path = media_queue_path(i);

    snprintf(text, (size_t)tn, "%s", path ? base_name(path) : "");
    if (i == media_queue_current() && media_queue_active()) {
        char t[12];

        time_text(t, sizeof(t), media_position());
        snprintf(detail, (size_t)dn, "%s%s", paused() ? "|| " : "", t);
    }
}

static const icon_t *item_icon(void *ctx, int i)
{
    return i == media_queue_current() && media_queue_active() ? ic_play : ic_music;
}

/* the selection follows the track playing (the queue moves on by itself) */
static void follow(void)
{
    int cur = media_queue_current();

    list.count = media_queue_count();
    if (cur != seen_cur) {
        seen_cur = cur;
        if (cur >= 0)
            list.sel = cur;
    }
    if (list.sel >= list.count)
        list.sel = list.top = 0;
}

static bool need_fill;                  /* /music is read after the first draw */

static void list_draw_(screen_t *s)
{
    char buf[48];
    int count = media_queue_count(), cur = media_queue_current();
    const char *err = media_queue_error();

    if (need_fill) {
        gfx_text_centered(UI_TOP + 40, "Reading /music...", C_DIM, FONT_BODY, 1);
        return;
    }
    follow();
    if (!count) {
        list.count = 0;
        gfx_text_centered(UI_TOP + 30, "No music", C_TEXT, FONT_LARGE, 1);
        gfx_text_centered(UI_TOP + 50, "Music files in /music,", C_DIM, FONT_SMALL, 1);
        gfx_text_centered(UI_TOP + 62, "or Center on one or an", C_DIM, FONT_SMALL, 1);
        gfx_text_centered(UI_TOP + 74, ".m3u in Files", C_DIM, FONT_SMALL, 1);
        if (!media_extensions()[0])
            gfx_text_centered(UI_TOP + 90, "(no codecs on the card)", C_BAD, FONT_SMALL, 1);
        return;
    }
    list_draw(&list);
    if (UI_TOP + count * UI_ROW_H > UI_BOTTOM - 14 && !err)
        return;                         /* no room under the rows */
    if (err)
        snprintf(buf, sizeof(buf), "%s", err);
    else
        snprintf(buf, sizeof(buf), "%d of %d  %s", cur >= 0 ? cur + 1 : 0, count,
                 media_queue_source());
    gfx_text(UI_PAD, UI_BOTTOM - 10, buf, err ? C_BAD : C_DIM, FONT_SMALL, 1,
             LCD_WIDTH - 2 * UI_PAD);
}

/* Quit: the queue and the notification go, which is what kept the app loaded even
 * when paused, then its screens; the loader unloads it. */
static void quit(void)
{
    int n = n_shown;

    media_queue_clear();
    notify_remove(&note);
    while (n-- > 0)
        ui_pop();
}

static const char *options[2];

static void option_picked(int i, void *ctx)
{
    if (i == 0)
        media_queue_set_shuffle(!media_queue_shuffle());
    else
        quit();
}

static void show_now_playing(void)
{
    screen_t *s;

    if (under_is(S_NP))
        ui_pop();                       /* now playing is under the list */
    else if ((s = new_screen(&np_tpl)))
        ui_push(s);
}

static void list_key_(screen_t *s, const key_event_t *ev)
{
    used();
    switch (ev->code) {
    case KEY_UP:
    case KEY_DOWN:
    case KEY_LEFT:
    case KEY_RIGHT:
        follow();
        list_key(&list, ev);
        break;
    case KEY_CENTER:
        if (!media_queue_count())
            break;
        if (!(media_queue_active() && list.sel == media_queue_current()))
            play(list.sel);
        seen_cur = media_queue_current();
        show_now_playing();
        break;
    case KEY_HS_CENTER:
        toggle_pause();
        break;
    case KEY_LSOFT:
        options[0] = media_queue_shuffle() ? "Shuffle: on" : "Shuffle: off";
        options[1] = "Quit";
        ui_choose("Options", options, 2, -1, option_picked, NULL);
        break;
    case KEY_RSOFT:
        ui_pop();                       /* the music plays on */
        break;
    }
}

static void list_enter(screen_t *s)
{
    shown_push(S_LIST);
    follow();
}

static void list_leave(screen_t *s)
{
    note_ensure();
    shown_pop();
}

static void fill(void);

static void list_tick(screen_t *s)
{
    if (idle_long()) {
        quit();
        return;
    }
    /* a redraw: the elapsed time in the list (the OS runs the queue) */
    note_ensure();
    if (need_fill) {                    /* the screen is up: now the scan */
        fill();
        need_fill = false;
        s->tick_ms = 500;
    }
}

static const screen_t list_tpl = {
    .title = "Music", .left = "Options", .center = "Play", .right = "Back",
    .enter = list_enter, .leave = list_leave, .draw = list_draw_, .key = list_key_,
    .tick = list_tick, .tick_ms = 500,
};

/* ---- opening ---------------------------------------------------------------- */

/* the queue: the song's folder, played from the song */
static void play_file(const char *path)
{
    char dir[PATH_MAX_];
    const char *slash = strrchr(path, '/');
    size_t dl = slash ? (size_t)(slash - path) : 0;
    int i;

    if (dl >= sizeof(dir))
        return;
    memcpy(dir, path, dl);
    dir[dl] = 0;
    media_queue_clear();
    media_queue_add_folder(dl ? dir : "/", 0);
    if ((i = media_queue_find(path)) < 0 && media_queue_add(path))
        i = media_queue_find(path);     /* not found by the scan: on its own */
    if (i >= 0)
        play(i);
    else
        toggle_pause();
}

/* an empty queue: /music and its folders, or else the recordings */
static void fill(void)
{
    if (media_queue_add_folder("/music", SCAN_DEPTH))
        return;
    media_queue_set_source("");
    if (!media_queue_add_folder(REC_DIR, 0))
        media_queue_set_source("/music");
}

static screen_t *start(void)
{
    const char *arg = app_arg();

    used();
    ic_play = ui_icon("play");
    ic_music = ui_icon("music");
    list.item = item;
    list.icon = item_icon;
    if (arg && arg[0]) {
        if (is_m3u(arg)) {
            media_queue_clear();
            media_queue_add_m3u(arg);
            toggle_pause();             /* nothing plays: the first in the order */
        } else {
            play_file(arg);
        }
        seen_cur = -2;
        return new_screen(&np_tpl);
    }
    if (media_queue_active())
        return new_screen(&np_tpl);
    need_fill = !media_queue_count();
    {
        /* the first time the folder is read (half a second with a full
         * card): the screen first, the scan at its first tick, 30 ms on */
        screen_t *s = new_screen(&list_tpl);

        if (s && need_fill)
            s->tick_ms = 30;
        return s;
    }
}

static const icon_t app_icon = APP_ICON_MUSIC;  /* declared above */
APP("Music", start, NULL, &app_icon);
