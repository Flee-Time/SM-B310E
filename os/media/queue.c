/* B310E OS — the play queue: a list of tracks the engine plays one after
 * the other, in order or shuffled, repeating all, one or none. It lives in
 * the OS, so the music goes on whatever app or screen is open: the UI loop
 * calls media_queue_tick() every 50 ms, and when a track ends the next
 * starts. Playback that something else stopped (a tone, the alarm, USB)
 * ends the queue's playing; the list stays. The Music app is its face:
 * it fills the queue (a folder, an .m3u, /music) and shows it.
 *
 * Until 2026-11-07 this was part of the built-in Music screen.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "os/fs.h"
#include "os/kernel.h"
#include "os/lib.h"
#include "os/media.h"
#include "apps/wavio.h"

#define MAX_TRACKS  512
#define PATH_MAX_   160
#define NAMES_BYTES (64 * 1024)
#define M3U_MAX     (16 * 1024)         /* the part of an .m3u read */

static char **tracks;                   /* full paths */
static char *names;                     /* one block for them */
static size_t names_used;
static int count;
int music_cur = -1;                     /* the track playing (or last played); */
bool music_on;                          /* the queue owns the playback (the tests read both) */
static uint16_t order[MAX_TRACKS];      /* play order: shuffled or not */
static bool shuffle;
static int repeat = MEDIA_REPEAT_ALL;
static int failures;                    /* tracks in a row that would not play */
static char source[48];                 /* where the list came from */
static uint32_t seed = 0x2545f491;
static const char *last_error;

static bool alloc(void)
{
    if (!tracks)
        tracks = kmalloc(sizeof(char *) * MAX_TRACKS);
    if (!names)
        names = kmalloc(NAMES_BYTES);
    return tracks && names;
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

static bool add(const char *dir, const char *name)
{
    size_t dl = strlen(dir), nl = strlen(name);
    bool slash = dl && dir[dl - 1] != '/';
    size_t need = dl + slash + nl + 1;

    if (!alloc() || count >= MAX_TRACKS || names_used + need > NAMES_BYTES || need > PATH_MAX_)
        return false;
    tracks[count] = names + names_used;
    memcpy(names + names_used, dir, dl);
    if (slash)
        names[names_used + dl] = '/';
    memcpy(names + names_used + dl + slash, name, nl + 1);
    names_used += need;
    order[count] = (uint16_t)count;
    count++;
    return true;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* by folder, then by name, case folded */
static int path_cmp(const char *a, const char *b)
{
    for (; *a && lower(*a) == lower(*b); a++, b++)
        ;
    return lower(*a) - lower(*b);
}

static void sort_from(int first)
{
    for (int i = first + 1; i < count; i++) {   /* insertion sort: few hundred */
        char *t = tracks[i];
        int j = i;

        for (; j > first && path_cmp(tracks[j - 1], t) > 0; j--)
            tracks[j] = tracks[j - 1];
        tracks[j] = t;
    }
}

static void make_order(void)
{
    for (int i = 0; i < count; i++)
        order[i] = (uint16_t)i;
    if (!shuffle)
        return;
    seed ^= kernel_ticks();
    for (int i = count - 1; i > 0; i--) {    /* Fisher-Yates, xorshift */
        int j;
        uint16_t t;

        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        j = (int)(seed % (uint32_t)(i + 1));
        t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    /* the track playing stays first, so the rest follows it */
    for (int k = 0; k < count && music_cur >= 0; k++)
        if (order[k] == music_cur) {
            order[k] = order[0];
            order[0] = (uint16_t)music_cur;
            break;
        }
}

/* every playable file in a folder, its folders too while depth > 0 */
static void scan(const char *dir, int depth)
{
    DIR d;
    FILINFO fi;
    int first = count;

    if (f_opendir(&d, dir) != FR_OK)
        return;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
        if (!(fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) && media_supported(fi.fname))
            add(dir, fi.fname);
    f_closedir(&d);
    sort_from(first);
    if (depth <= 0 || f_opendir(&d, dir) != FR_OK)
        return;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        char sub[PATH_MAX_];

        if (!(fi.fattrib & AM_DIR) || fi.fattrib & (AM_HID | AM_SYS) || fi.fname[0] == '.')
            continue;
        if ((size_t)snprintf(sub, sizeof(sub), "%s%s%s", dir,
                             dir[strlen(dir) - 1] == '/' ? "" : "/", fi.fname) >= sizeof(sub))
            continue;
        scan(sub, depth - 1);
    }
    f_closedir(&d);
}

static int play_index(int i)
{
    if (i < 0 || i >= count)
        return -1;
    music_cur = i;
    if (wav_playing() || wav_recording())
        wav_stop();                     /* a recording playing */
    music_on = media_play(tracks[i]) == 0;
    last_error = music_on ? NULL : "the player did not start";
    return music_on ? 0 : -1;
}

/* the position of the current track in the play order */
static int order_pos(void)
{
    for (int k = 0; k < count; k++)
        if (order[k] == music_cur)
            return k;
    return 0;
}

/* the track ended (or would not play): what comes next */
static void next_track(bool ended)
{
    int k = order_pos();

    if (ended && repeat == MEDIA_REPEAT_ONE) {
        play_index(music_cur);
    } else if (repeat == MEDIA_REPEAT_OFF && k + 1 >= count) {
        music_on = false;               /* the end of the list */
        media_stop();
    } else {
        play_index(order[(k + 1) % count]);
    }
}

/* ---- the interface (os/media.h) ------------------------------------------------- */

void media_queue_clear(void)
{
    if (music_on) {
        music_on = false;
        media_stop();
    }
    count = 0;
    music_cur = -1;
    names_used = 0;
    source[0] = 0;
    failures = 0;
    last_error = NULL;
}

bool media_queue_add(const char *path)
{
    const char *slash = strrchr(path, '/');

    if (!slash)
        return add("", path);
    {
        char dir[PATH_MAX_];
        size_t dl = (size_t)(slash - path);

        if (dl >= sizeof(dir))
            return false;
        memcpy(dir, path, dl);
        dir[dl] = 0;
        return add(dl ? dir : "/", slash + 1);
    }
}

int media_queue_add_folder(const char *dir, int depth)
{
    int before = count;

    if (!alloc())
        return 0;
    scan(dir, depth);
    if (!source[0])
        snprintf(source, sizeof(source), "%s", dir);
    make_order();
    return count - before;
}

int media_queue_add_m3u(const char *path)
{
    char dir[PATH_MAX_], *buf, *p;
    const char *slash = strrchr(path, '/');
    size_t dl = slash ? (size_t)(slash - path) : 0;
    FIL f;
    UINT got = 0;
    int skipped = 0, before = count;

    if (dl >= sizeof(dir) || !alloc())
        return 0;
    memcpy(dir, path, dl);
    dir[dl] = 0;
    buf = kmalloc(M3U_MAX + 1);
    if (!buf || f_open(&f, path, FA_READ) != FR_OK) {
        kfree(buf);
        return 0;
    }
    f_read(&f, buf, M3U_MAX, &got);
    f_close(&f);
    buf[got] = 0;
    for (p = buf; *p; ) {
        char *line = p;
        size_t n;

        while (*p && *p != '\n')
            p++;
        if (*p)
            *p++ = 0;
        n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = 0;
        if (n >= PATH_MAX_)
            continue;
        for (size_t i = 0; i < n; i++)  /* Windows lists use backslashes */
            if (line[i] == '\\')
                line[i] = '/';
        if (!n || line[0] == '#')
            continue;
        if (!media_supported(line) || is_m3u(line)) {
            skipped++;
            continue;
        }
        if (line[0] == '/')
            add("", line);              /* absolute: the path as it is */
        else
            add(dl ? dir : "/", line);
    }
    kfree(buf);
    snprintf(source, sizeof(source), "%s", base_name(path));
    klog("music: %s, %d tracks%s", path, count - before,
         skipped ? " (unplayable ones left out)" : "");
    make_order();
    return count - before;
}

int media_queue_count(void)
{
    return count;
}

const char *media_queue_path(int i)
{
    return i >= 0 && i < count ? tracks[i] : NULL;
}

int media_queue_find(const char *path)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(tracks[i], path))
            return i;
    return -1;
}

int media_queue_current(void)
{
    return music_cur;
}

bool media_queue_active(void)
{
    return music_on;
}

int media_queue_play(int i)
{
    failures = 0;
    return play_index(i);
}

void media_queue_step(int d)
{
    if (count)
        play_index(order[(order_pos() + d + count) % count]);
}

/* Center: pause or resume; nothing playing: play (the current track, or
 * the first in the order) */
void media_queue_toggle(void)
{
    if (!music_on) {
        if (count)
            play_index(music_cur >= 0 ? music_cur : order[0]);
    } else {
        media_pause(media_state() != MEDIA_PAUSED);
    }
}

void media_queue_set_shuffle(bool on)
{
    shuffle = on;
    make_order();
}

bool media_queue_shuffle(void)
{
    return shuffle;
}

void media_queue_set_repeat(int mode)
{
    if (mode >= MEDIA_REPEAT_ALL && mode <= MEDIA_REPEAT_OFF)
        repeat = mode;
}

int media_queue_repeat(void)
{
    return repeat;
}

const char *media_queue_source(void)
{
    return source;
}

void media_queue_set_source(const char *s)
{
    snprintf(source, sizeof(source), "%s", s ? s : "");
}

const char *media_queue_error(void)
{
    return last_error;
}

/* the name of the track playing, or NULL */
const char *media_queue_now(void)
{
    return music_on && music_cur >= 0 ? base_name(tracks[music_cur]) : NULL;
}

void media_queue_tick(void)
{
    if (!music_on)
        return;
    switch (media_state()) {
    case MEDIA_PLAYING:
        failures = 0;
        last_error = NULL;
        break;
    case MEDIA_ENDED:
        next_track(true);
        break;
    case MEDIA_ERROR:
        last_error = media_error();
        klog("music: %s: %s", tracks[music_cur], last_error);
        if (++failures >= count) {      /* nothing here plays: stop */
            music_on = false;
            failures = 0;
        } else {
            next_track(false);
        }
        break;
    case MEDIA_STOPPED:                 /* something else took the audio */
    case MEDIA_IDLE:
        music_on = false;
        break;
    default:
        break;
    }
}
