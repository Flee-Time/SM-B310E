/* The game kit, the OS's side: the app's screen and threads, frames to
 * the LCD (turned and scaled by the view), keys, sound, files, the game's
 * block of memory. A game provides kit_game (kit_app.h) and its main();
 * see kit.h for what the game's code sees.
 *
 * The game runs in a thread of the app's at PRIO_LOW (every OS thread
 * first), its sound in another above it. Its screen is full-screen,
 * raw-key and awake (ui_screen_mode); the game ending (or an error, shown
 * with Back) pops it from the UI thread (ui_call).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "app.h"
#include "kit.h"
#include "kit_app.h"
#include "kit_os.h"

#define SOUND_STACK   (16 * 1024)       /* the game renders in it: Wolf4SDL's OPL takes 4 KB alone */
#define RING_FRAMES   4096              /* the sound ring: 186 ms at 22 kHz */
#define RING_TARGET   2048              /* rendered ahead */
#define RENDER_CHUNK  256
#define MAX_ARGS      16

const os_api_t *os;                     /* the SDK's start.c is not linked:
                                         * kit_sys.c has memcpy and memset */

/* os/tests read frames (+0), error (+4) and arena (+164): keep them first */
struct kit_state kit = { 0 };

static struct {
    char dir[64];
    char *argv[MAX_ARGS];
    char strings[512];
    unsigned used;
    int argc;
    thread_t *thread;
    volatile bool running, on_stack, quit_asked, held, paused;
    volatile bool dark;                 /* stopped: the screen is dark */
    uint32_t dark_ms;                   /* time the game did not see */
    int depth;                          /* ui_depth with its screen on top */
    struct { uint8_t code, down; } keys[64];
    volatile unsigned key_in, key_out;
    kit_view_t view;
    uint32_t spread[256];               /* the palette: r << 22 | b << 11 | g << 1 */
    mutex_t frame_lock;
    /* Two frames (128 x 160, the LCD's order): the game draws in one while the
     * LCD reads the other, and the UI swaps them, with no copy. */
    void *frames;
    uint16_t *frame;                    /* being drawn in, or finished: ready */
    uint16_t *ready;                    /* finished, for the LCD; drawn over if a newer comes first */
    uint16_t *shown;                    /* on the LCD (only the UI thread writes it) */
} k;

/* ---- the game's arguments and folder (prepare, in the UI thread) ---------------- */
void kit_arg(const char *s)
{
    size_t n = strlen(s) + 1;

    if (k.argc >= MAX_ARGS - 1 || k.used + n > sizeof(k.strings))
        return;
    memcpy(k.strings + k.used, s, n);
    k.argv[k.argc++] = k.strings + k.used;
    k.argv[k.argc] = NULL;
    k.used += n;
}

void kit_set_dir(const char *dir)
{
    snprintf(k.dir, sizeof(k.dir), "%s", dir);
}

void kit_fail(const char *text)
{
    snprintf(kit.error, sizeof(kit.error), "%s", text);
}

const char *kit_dir(void) { return k.dir; }
void kos_error(const char *text) { kit_fail(text); }

/* ---- the view: where each frame pixel goes --------------------------------------- */
void kit_set_view(const kit_view_t *v)
{
    k.view = *v;
    if (k.view.rotate > KIT_ROT_270)
        k.view.rotate = KIT_ROT_0;
}

void kit_get_view(kit_view_t *v) { *v = k.view; }

void kit_palette(const uint8_t *rgb, int n)
{
    for (int i = 0; i < n && i < 256; i++, rgb += 3)
        k.spread[i] = (uint32_t)rgb[0] << 22 | (uint32_t)rgb[2] << 11 | (uint32_t)rgb[1] << 1;
}

/* The game's w x h onto the screen as the view turns it (gw x gh): for each
 * screen column (row) the frame's column (row) and the one beside it when
 * scaling down (else itself), and the part of the screen covered. */
typedef struct {
    int16_t at[KIT_LCD_H], next[KIT_LCD_H];
    int lo, hi;                         /* covered: lo <= i < hi */
} axis_t;

static void axis(axis_t *a, int src, int dst, int scaled, bool smooth)
{
    int off = (dst - scaled) / 2;       /* <0: cropped, >0: bars */
    int32_t quarter = (int32_t)(((int64_t)src << 14) / scaled); /* of a screen pixel, 16.16 */
    int merges = src - scaled;

    a->lo = off > 0 ? off : 0;
    a->hi = off > 0 ? off + scaled : dst;
    for (int i = a->lo; i < a->hi; i++) {
        /* the centre of screen pixel i in the frame, 16.16 */
        int32_t c = (int32_t)(((int64_t)(2 * (i - off) + 1) * src << 15) / scaled);
        int x = c >> 16;

        if (x >= src)
            x = src - 1;
        a->at[i] = (int16_t)x;
        a->next[i] = (int16_t)x;
        if (smooth && merges > 0 && merges <= scaled) {
            /* src lines in scaled, every line kept: the many as they are,
             * sharp, and src - scaled pairs, evenly spread, each the mean of
             * two. Only those can look soft. */
            int n = i - off, done = (n * merges + scaled / 2) / scaled;
            int after = ((n + 1) * merges + scaled / 2) / scaled;

            x = n + done;
            a->at[i] = (int16_t)(x < src ? x : src - 1);
            a->next[i] = (int16_t)(x + (after > done) < src ? x + (after > done) : src - 1);
        } else if (smooth && merges > scaled) { /* smaller than half: two of what it covers */
            int x0 = (c - quarter) >> 16, x1 = (c + quarter) >> 16;

            a->at[i] = (int16_t)(x0 < 0 ? 0 : x0);
            a->next[i] = (int16_t)(x1 >= src ? src - 1 : x1);
        }
    }
}

/* A console drawing in tiles (kit_game.row_grid): its fonts leave the last
 * row of a tile empty. The rows to lose are those, a tile's last taken with
 * the one before (keep_detail), not wherever they fall evenly: a letter
 * keeps every line, sharp. */
static void grid_rows(axis_t *a, int src, int dst, int scaled, int grid)
{
    int merges = src - scaled, cells = src / grid, off = (dst - scaled) / 2, i = off > 0 ? off : 0;

    if (merges <= 0 || merges > cells || src % grid || off < 0)
        return;
    for (int cell = 0; cell < cells; cell++) {
        bool loses = ((cell + 1) * merges + cells / 2) / cells > (cell * merges + cells / 2) / cells;

        for (int r = 0; r < grid - (loses ? 1 : 0); r++, i++) {
            a->at[i] = (int16_t)(cell * grid + r);
            a->next[i] = (int16_t)(cell * grid + r + (loses && r == grid - 2));
        }
    }
}

/* kit_present565 across: up to 3 pixels of a row to a screen pixel, in
 * sixteenths. From 1.5 pixels to one on it is what each covers, exactly: a
 * thin line weighs the same wherever a scroll puts it (with 2 samples it
 * went from thin to thick). Below that as axis() has it. */
typedef struct {
    int16_t x;
    uint8_t w0, w1, w2;
} tap_t;

static tap_t taps[KIT_LCD_H];
static bool halved;                     /* across, the frame is exactly twice what it shows as */

static void make_taps(const axis_t *a, int src, int dst, int scaled, bool smooth)
{
    int off = (dst - scaled) / 2;
    bool area = smooth && 2 * src >= 3 * scaled && src <= 2 * scaled;

    for (int i = a->lo; i < a->hi; i++) {
        tap_t *t = &taps[i];

        t->x = a->at[i];
        t->w0 = a->at[i] == a->next[i] ? 16 : 8;
        t->w1 = (uint8_t)(16 - t->w0);
        t->w2 = 0;
        if (area) {
            /* the screen pixel covers [lo, hi) of the row, in 1/4096ths */
            int lo = (i - off) * 4096 * src / scaled, hi = (i - off + 1) * 4096 * src / scaled;
            int x = lo / 4096, w0, w2 = 0;

            w0 = (((x + 1) * 4096 - lo) * 16 + (hi - lo) / 2) / (hi - lo);
            if (hi > (x + 2) * 4096)
                w2 = ((hi - (x + 2) * 4096) * 16 + (hi - lo) / 2) / (hi - lo);
            if (x + 2 >= src || w2 < 0)
                w2 = 0;
            t->x = (int16_t)x;
            t->w0 = (uint8_t)w0;
            t->w1 = (uint8_t)(16 - w0 - w2);
            t->w2 = (uint8_t)w2;
        }
    }
}

static axis_t ax_u, ax_v;
static uint32_t kit_sound_cycles(void);
static uint32_t launched_ms, fps_since, fps_frames, scale_ms;
static uint32_t frame_at, worst_ms, slow_frames, heap_low = ~0u;

/* the log says how long a game took to its first frame, then its frames
 * a second every 10 s: the phone's numbers */
/* where the phone's time went, for the log: the game's thread and the sound's, of
 * each 10 s, in the kernel's 26 MHz cycles */
static void cpu_report(uint32_t ms)
{
    static uint32_t game_last, sound_last;
    uint32_t game = k.thread ? k.thread->cycles : 0, sound = kit_sound_cycles();

    if (ms)
        klog("%s: of the time: game %lu%%, sound %lu%%", kit_game.log_name,
             (unsigned long)((game - game_last) / 26000 * 100 / ms),
             (unsigned long)((sound - sound_last) / 26000 * 100 / ms));
    game_last = game;
    sound_last = sound;
}

static void frame_done(void)
{
    uint32_t now = ticks();

    if (frame_at) {
        if (now - frame_at > worst_ms)
            worst_ms = now - frame_at;
        if (now - frame_at > 33)
            slow_frames++;
    }
    frame_at = now;
    if (heap_free() < heap_low)
        heap_low = heap_free();
    if (kit.frames == 1) {
        klog("%s: first frame after %lu ms", kit_game.log_name,
             (unsigned long)(now - launched_ms));
        kit_io_report("to the first frame");
        fps_since = now;
        fps_frames = 1;
    } else if (now - fps_since >= 10000) {
        uint32_t tenths = (kit.frames - fps_frames) * 10000u / (now - fps_since);

        static unsigned last_kb, last_ms;
        unsigned kb = kit_io_kb(), ms = kit_io_ms();

        cpu_report(now - fps_since);

        klog("%s: %lu.%lu fps, %lu.%lu ms a frame to scale, %u KB read in %u ms, "
             "worst frame %lu ms, %lu over 33 ms", kit_game.log_name,
             (unsigned long)(tenths / 10), (unsigned long)(tenths % 10),
             (unsigned long)(scale_ms / (kit.frames - fps_frames)),
             (unsigned long)(scale_ms * 10 / (kit.frames - fps_frames) % 10), kb - last_kb,
             ms - last_ms, (unsigned long)worst_ms, (unsigned long)slow_frames);
        last_kb = kb;
        last_ms = ms;
        worst_ms = slow_frames = 0;
        fps_since = now;
        fps_frames = kit.frames;
        scale_ms = 0;
    }
}
static int last_w, last_h, last_rot = -1, last_scale = -1, last_smooth = -1;

static void geometry(int w, int h, int *base, int *du, int *dv, int *gw, int *gh)
{
    switch (k.view.rotate) {
    default:
    case KIT_ROT_0:   *base = 0; *du = 1; *dv = KIT_LCD_W; break;
    case KIT_ROT_90:  *base = KIT_LCD_W - 1; *du = KIT_LCD_W; *dv = -1; break;
    case KIT_ROT_180: *base = KIT_LCD_W * KIT_LCD_H - 1; *du = -1; *dv = -KIT_LCD_W; break;
    case KIT_ROT_270: *base = KIT_LCD_W * (KIT_LCD_H - 1); *du = -KIT_LCD_W; *dv = 1; break;
    }
    *gw = k.view.rotate & 1 ? KIT_LCD_H : KIT_LCD_W;
    *gh = k.view.rotate & 1 ? KIT_LCD_W : KIT_LCD_H;
    if (w == last_w && h == last_h && k.view.rotate == last_rot &&
        k.view.scale == last_scale && k.view.smooth == last_smooth)
        return;
    {
        int sw, sh;                     /* the frame's size on the screen */

        switch (k.view.scale) {
        case KIT_FILL:
            sw = *gw;
            sh = *gh;
            break;
        case KIT_ONE:
            sw = w;
            sh = h;
            break;
        case KIT_CROP:                  /* the larger of the two scales */
            if (w * *gh > h * *gw) {
                sh = *gh;
                sw = w * *gh / h;
            } else {
                sw = *gw;
                sh = h * *gw / w;
            }
            break;
        default:                        /* KIT_FIT: the smaller */
            if (w * *gh > h * *gw) {
                sw = *gw;
                sh = h * *gw / w;
            } else {
                sh = *gh;
                sw = w * *gh / h;
            }
            if (2 * w >= 3 * sw && w <= 2 * sw) {   /* near half across: half exactly, so
                                         * no column is ever heavier than another
                                         * when it scrolls. Downwards it still
                                         * fills the screen, if it did. */
                sw = w / 2;
                if (2 * sh < h)
                    sh = h / 2;
            }
            break;
        }
        axis(&ax_u, w, *gw, sw, k.view.smooth);
        axis(&ax_v, h, *gh, sh, k.view.smooth);
        if (k.view.smooth && kit_game.row_grid)
            grid_rows(&ax_v, h, *gh, sh, kit_game.row_grid);
        make_taps(&ax_u, w, *gw, sw, k.view.smooth);
        halved = k.view.smooth && w == 2 * sw;
    }
    last_w = w;
    last_h = h;
    last_rot = k.view.rotate;
    last_scale = k.view.scale;
    last_smooth = k.view.smooth;
    memset(k.frame, 0, KIT_LCD_W * KIT_LCD_H * 2);  /* the bars */
    if (k.shown)
        memset(k.shown, 0, KIT_LCD_W * KIT_LCD_H * 2);
}

static inline uint16_t mean4(uint32_t a)
{
    a += (0x00400802u & a >> 2) + 0x00400802u;
    a &= 0xf81f07e0u;
    return (uint16_t)(a | a >> 16);
}

static void dark_pause(void);
static void sound_back(void);

/* "Leave?" is up, or the screen is dark: the game waits here */
static void wait_if_paused(void)
{
    for (;;) {
        if (ui_is_dark())
            dark_pause();
        if (!k.paused || k.quit_asked)
            break;
        thread_sleep(20);
    }
    /* Something else took the sound (an alarm's tone) and has gone: it comes back
     * when the game's screen is the top one again */
    sound_back();
}

void kit_present8(const uint8_t *src, int w, int h, int pitch)
{
    int base, du, dv, gw, gh;
    const uint32_t *pal = k.spread;

    wait_if_paused();
    mutex_lock(&k.frame_lock, WAIT_FOREVER);
    if (k.frame) {
        uint32_t start = ticks();

        geometry(w, h, &base, &du, &dv, &gw, &gh);
        for (int v = ax_v.lo; v < ax_v.hi; v++) {
            const uint8_t *r0 = src + ax_v.at[v] * pitch, *r1 = src + ax_v.next[v] * pitch;
            uint16_t *d = k.frame + base + v * dv + ax_u.lo * du;

            if (r0 == r1) {                     /* one row: half the look-ups */
                for (int u = ax_u.lo; u < ax_u.hi; u++, d += du)
                    *d = mean4((pal[r0[ax_u.at[u]]] + pal[r0[ax_u.next[u]]]) * 2);
            } else {
                for (int u = ax_u.lo; u < ax_u.hi; u++, d += du) {
                    int x0 = ax_u.at[u], x1 = ax_u.next[u];

                    *d = mean4(pal[r0[x0]] + pal[r0[x1]] + pal[r1[x0]] + pal[r1[x1]]);
                }
            }
        }
        scale_ms += ticks() - start;
        k.ready = k.frame;
        kit.frames++;
        frame_done();
    }
    mutex_unlock(&k.frame_lock);
    ui_refresh();
}

/* A pixel in a word with room to weigh it: green | red | blue */
static inline uint32_t apart(uint32_t c) { return (c | c << 16) & 0x07e0f81fu; }

static inline uint16_t together(uint32_t c)
{
    c = (c + 0x01004008u) >> 4 & 0x07e0f81fu;   /* rounded */
    return (uint16_t)(c | c >> 16);
}

/* two rows, the mean of each pixel; a word's two at a time when they can be */
static uint16_t row_mean[320] __attribute__((aligned(4)));

static const uint16_t *mean_rows(const uint16_t *r0, const uint16_t *r1, int w)
{
    if (!(((uintptr_t)r0 | (uintptr_t)r1) & 3)) {
        const uint32_t *a = (const uint32_t *)(const void *)r0, *b = (const uint32_t *)(const void *)r1;
        uint32_t *d = (uint32_t *)(void *)row_mean;

        for (int x = 0; x < (w + 1) / 2; x++)
            d[x] = (a[x] & b[x]) + ((a[x] ^ b[x]) >> 1 & 0x7bef7befu);
    } else {
        for (int x = 0; x < w; x++)
            row_mean[x] = (uint16_t)((r0[x] & r1[x]) + ((r0[x] ^ r1[x]) >> 1 & 0x7befu));
    }
    return row_mean;
}

/* A tile's last row into the one before: where it is what the row before or
 * after it is, it is empty and adds nothing (the row stays sharp); else it
 * has something of its own (a thin line in a picture), and that is not lost. */
static const uint16_t *keep_detail(const uint16_t *r0, const uint16_t *r1, const uint16_t *r2, int w)
{
    for (int x = 0; x < w; x++)
        row_mean[x] = r1[x] == r0[x] || r1[x] == r2[x] ? r0[x]
                    : (uint16_t)((r0[x] & r1[x]) + ((r0[x] ^ r1[x]) >> 1 & 0x7befu));
    return row_mean;
}

/* A row to the screen, across as the taps say */
static inline void row_out(const uint16_t *row, uint16_t *d, int du)
{
    const tap_t *t = &taps[ax_u.lo], *end = &taps[ax_u.hi];

    for (; t < end; t++, d += du) {
        if (t->w0 == 16)
            *d = row[t->x];
        else if (!t->w2)
            *d = together(apart(row[t->x]) * t->w0 + apart(row[t->x + 1]) * t->w1);
        else
            *d = together(apart(row[t->x]) * t->w0 + apart(row[t->x + 1]) * t->w1 +
                          apart(row[t->x + 2]) * t->w2);
    }
}

/* Two rows, each pair of pixels to one (exactly half): a word's two pixels at
 * a time, green's lowest bit left out, for room (InfoNES's flag is there) */
static inline void half_out(const uint32_t *a, const uint32_t *b, uint16_t *d, int du)
{
    for (int u = ax_u.lo; u < ax_u.hi; u++, d += du, a++, b++) {
        uint32_t p = *a, q = *b, c;

        c = (p & 0x07c0f81fu) + (q & 0x07c0f81fu) +
            ((p >> 16 | p << 16) & 0x07c0f81fu) + ((q >> 16 | q << 16) & 0x07c0f81fu);
        c = (c + 0x00801002u) >> 2 & 0x07c0f81fu;       /* rounded */
        *d = (uint16_t)(c | c >> 16);
    }
}

void kit_present565(const uint16_t *src, int w, int h, int pitch)
{
    int base, du, dv, gw, gh;

    wait_if_paused();
    mutex_lock(&k.frame_lock, WAIT_FOREVER);
    if (k.frame) {
        uint32_t start = ticks();

        geometry(w, h, &base, &du, &dv, &gw, &gh);
        for (int v = ax_v.lo; v < ax_v.hi; v++) {
            const uint16_t *r0 = src + ax_v.at[v] * pitch, *r1 = src + ax_v.next[v] * pitch;
            uint16_t *d = k.frame + base + v * dv + ax_u.lo * du;
            const uint16_t *row = r0;

            if (halved && !(((uintptr_t)r0 | (uintptr_t)r1) & 3)) {     /* (r0 may be r1) */
                half_out((const uint32_t *)(const void *)r0, (const uint32_t *)(const void *)r1, d, du);
                continue;
            }
            if (r0 != r1 && w <= (int)ARRAY_SIZE(row_mean)) {
                if (kit_game.row_grid)
                    row = keep_detail(r0, r1, ax_v.next[v] + 1 < h ? r1 + pitch : r1, w);
                else
                    row = mean_rows(r0, r1, w);
            }
            row_out(row, d, du);
        }
        scale_ms += ticks() - start;
        k.ready = k.frame;
        kit.frames++;
        frame_done();
    }
    mutex_unlock(&k.frame_lock);
    ui_refresh();
}

/* The same a line at a time, as a console draws them: no frame of the
 * console's own to fill and read back. kit_line565 gets the lines top to bottom
 * and may use the last one's memory again: it keeps its pointer to mean with
 * the next, so two buffers in turn will do. */
static struct {
    int base, du, dv, w, v;
    const uint16_t *prev;
} scan;

void kit_begin565(int w, int h)
{
    int gw, gh;

    wait_if_paused();
    mutex_lock(&k.frame_lock, WAIT_FOREVER);
    if (k.frame)
        geometry(w, h, &scan.base, &scan.du, &scan.dv, &gw, &gh);
    scan.w = w;
    scan.v = ax_v.lo;
    scan.prev = NULL;
}

void kit_line565(int y, const uint16_t *line)
{
    while (k.frame && scan.v < ax_v.hi && ax_v.next[scan.v] <= y) {
        int v = scan.v++;
        uint16_t *d = k.frame + scan.base + v * scan.dv + ax_u.lo * scan.du;

        const uint16_t *other = ax_v.at[v] == y || !scan.prev ? line : scan.prev;

        if (halved)                     /* (other may be line) */
            half_out((const uint32_t *)(const void *)other,
                     (const uint32_t *)(const void *)line, d, scan.du);
        else if (other == line)
            row_out(line, d, scan.du);
        else
            row_out(mean_rows(other, line, scan.w), d, scan.du);
    }
    scan.prev = line;
}

void kit_end565(void)
{
    if (k.frame) {
        k.ready = k.frame;
        kit.frames++;
        frame_done();
    }
    mutex_unlock(&k.frame_lock);
    ui_refresh();
}

/* ---- keys ---------------------------------------------------------------------------- */
static int turn(int code)
{
    static const uint8_t dpad[4] = { KIT_UP, KIT_RIGHT, KIT_DOWN, KIT_LEFT };   /* clockwise */
    int i, r = k.view.keys == KIT_KEYS_AS_IS ? 0 : k.view.rotate;

    for (i = 0; i < 4 && dpad[i] != code; i++)
        ;
    if (i == 4 || !r)
        return code;
    /* the screen turned r quarters clockwise for the game: the game's up is
     * the phone's (up turned r quarters clockwise) */
    return dpad[(i + 4 - r) % 4];
}

bool kit_key(int *code, bool *down)
{
    unsigned out = k.key_out;

    if (out == k.key_in)
        return false;
    *code = turn(k.keys[out % ARRAY_SIZE(k.keys)].code);
    *down = k.keys[out % ARRAY_SIZE(k.keys)].down;
    k.key_out = out + 1;
    return true;
}

/* ---- sound ----------------------------------------------------------------------- */
static struct {
    void (*render)(int16_t *lr, unsigned frames);
    int16_t ring[RING_FRAMES * 2];
    volatile unsigned in, out;          /* frames, free-running */
    sem_t more;
    thread_t *thread;
    volatile bool on, stop;
    unsigned rate;
} snd;

static unsigned sound_fill(int16_t *lr, unsigned frames, void *ctx)
{
    unsigned have = snd.in - snd.out, n = frames < have ? frames : have;

    for (unsigned i = 0; i < n; i++) {
        unsigned at = (snd.out + i) % RING_FRAMES;

        lr[2 * i] = snd.ring[2 * at];
        lr[2 * i + 1] = snd.ring[2 * at + 1];
    }
    for (unsigned i = n; i < frames; i++)
        lr[2 * i] = lr[2 * i + 1] = 0;  /* behind (loading): silence */
    snd.out += n;
    sem_post(&snd.more);
    return frames;
}

static void sound_main(void *arg)
{
    int16_t chunk[RENDER_CHUNK * 2];

    while (!snd.stop) {
        while (!snd.stop && snd.in - snd.out < RING_TARGET) {
            snd.render(chunk, RENDER_CHUNK);
            for (unsigned i = 0; i < RENDER_CHUNK; i++) {
                unsigned at = (snd.in + i) % RING_FRAMES;

                snd.ring[2 * at] = chunk[2 * i];
                snd.ring[2 * at + 1] = chunk[2 * i + 1];
            }
            snd.in += RENDER_CHUNK;
        }
        sem_wait(&snd.more, k.dark ? 500 : 20);
    }
    snd.thread = NULL;
}

static uint32_t kit_sound_cycles(void)
{
    return snd.thread ? snd.thread->cycles : 0;
}

int kit_audio_start(unsigned rate, void (*render)(int16_t *lr, unsigned frames))
{
    if (snd.on)
        return 0;
    if (!audio_rate_ok(rate))
        return -1;
    snd.render = render;
    snd.rate = rate;
    snd.in = snd.out = 0;
    snd.stop = false;
    sem_init(&snd.more, 0);
    if (media_state() == MEDIA_PLAYING)
        media_pause(true);              /* the game's sound instead */
    wav_stop();
    snd.thread = thread_create("game-snd", PRIO_NORMAL - 1, SOUND_STACK, sound_main, NULL);
    if (!snd.thread)
        return -1;
    if (audio_play(rate, sound_fill, NULL)) {
        snd.stop = true;
        return -1;
    }
    snd.on = true;
    return 0;
}

void kit_audio_stop(void)
{
    if (!snd.on)
        return;
    snd.on = false;
    audio_play_stop();
    snd.stop = true;
    sem_post(&snd.more);
    /* it ends within a chunk (unless it was ended: END held) */
    for (int i = 0; snd.thread && i < 40; i++)
        thread_sleep(5);
}

static void sound_back(void)
{
    if (snd.on && !audio_playing() && k.on_stack && ui_depth() == k.depth)
        audio_play(snd.rate, sound_fill, NULL);
}

/* The screen went dark with the game running (no key for awake_limit_s, or it
 * is under another screen): nothing to see, so nothing to run. Its sound and its
 * hold on the clock go, so the phone can reach dark idle, and its clock stops:
 * any key wakes the screen and the game goes on from where it was. */
static void dark_pause(void)
{
    uint32_t t = ticks();
    bool held = k.held;

    klog("%s: paused, the screen is dark", kit_game.log_name);
    k.dark = true;
    if (snd.on)
        audio_play_stop();
    if (held) {
        k.held = false;
        cpufreq_release();
    }
    while (ui_is_dark() && !k.quit_asked)
        thread_sleep(200);
    if (held) {
        cpufreq_hold();
        k.held = true;
    }
    k.dark = false;
    if (snd.on && !audio_playing() && audio_play(snd.rate, sound_fill, NULL))
        klog("%s: no sound after the pause", kit_game.log_name);
    k.dark_ms += ticks() - t;
    klog("%s: going on after %lu s", kit_game.log_name, (unsigned long)((ticks() - t) / 1000));
}

/* ---- the rest of kit.h ------------------------------------------------------------ */
uint32_t kit_ms(void) { return ticks() - k.dark_ms; }
void kit_sleep(uint32_t ms) { thread_sleep(ms); }
bool kit_quit_asked(void) { return k.quit_asked; }
void kit_log(const char *s) { klog("%s: %s", kit_game.log_name, s); }

/* ARM state for the CPSR: Thumb has no mrs/msr (asm in kit_asm.S) */
unsigned kit_irq_save_arm(void);
void kit_irq_restore_arm(unsigned s);
unsigned kit_irq_save(void) { return kit_irq_save_arm(); }
void kit_irq_restore(unsigned s) { kit_irq_restore_arm(s); }

/* ---- files (kit_os.h) --------------------------------------------------------------- */
int kos_file_size(void) { return (int)sizeof(FIL); }

int kos_open(void *f, const char *path, int mode)
{
    BYTE m = 0;

    if (mode & KOS_READ)
        m |= FA_READ;
    if (mode & KOS_WRITE)
        m |= FA_WRITE;
    if (mode & KOS_CREATE)
        m |= FA_CREATE_ALWAYS;
    else if (mode & KOS_NEW)
        m |= FA_CREATE_NEW;
    else if (mode & KOS_ALWAYS)
        m |= FA_OPEN_ALWAYS;
    return f_open(f, path, m) == FR_OK ? 0 : -1;
}

int kos_read(void *f, void *buf, unsigned n, unsigned *got)
{
    UINT k_ = 0;
    FRESULT r = f_read(f, buf, n, &k_);

    *got = k_;
    return r == FR_OK ? 0 : -1;
}

int kos_write(void *f, const void *buf, unsigned n, unsigned *put)
{
    UINT k_ = 0;
    FRESULT r = f_write(f, buf, n, &k_);

    *put = k_;
    return r == FR_OK ? 0 : -1;
}

int kos_seek(void *f, unsigned long pos) { return f_lseek(f, pos) == FR_OK ? 0 : -1; }
unsigned long kos_tell(void *f) { return (unsigned long)f_tell((FIL *)f); }
unsigned long kos_size(void *f) { return (unsigned long)f_size((FIL *)f); }
int kos_sync(void *f) { return f_sync(f) == FR_OK ? 0 : -1; }
int kos_close(void *f) { return f_close(f) == FR_OK ? 0 : -1; }
int kos_unlink(const char *path) { return f_unlink(path) == FR_OK ? 0 : -1; }
int kos_rename(const char *from, const char *to) { return f_rename(from, to) == FR_OK ? 0 : -1; }
int kos_mkdir(const char *path) { return f_mkdir(path) == FR_OK ? 0 : -1; }

static uint32_t epoch(int y, int mo, int d, int h, int mi, int s)
{
    static const uint16_t before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    uint32_t days = (uint32_t)(y - 1970) * 365 + (uint32_t)((y - 1969) / 4) +
                    before[(mo - 1) % 12] + (uint32_t)(d - 1);

    if (mo > 2 && y % 4 == 0)
        days++;
    return days * 86400u + (uint32_t)(h * 3600 + mi * 60 + s);
}

int kos_stat(const char *path, unsigned long *size, int *is_dir, uint32_t *mtime)
{
    FILINFO fi;

    if (f_stat(path, &fi) != FR_OK)
        return -1;
    *size = (unsigned long)fi.fsize;
    *is_dir = !!(fi.fattrib & AM_DIR);
    *mtime = epoch(1980 + (fi.fdate >> 9), (fi.fdate >> 5) & 15 ? (fi.fdate >> 5) & 15 : 1,
                   fi.fdate & 31 ? fi.fdate & 31 : 1, fi.ftime >> 11, (fi.ftime >> 5) & 63,
                   (fi.ftime & 31) * 2);
    return 0;
}

int kos_dir_size(void) { return (int)sizeof(DIR); }
int kos_opendir(void *d, const char *path) { return f_opendir(d, path) == FR_OK ? 0 : -1; }
void kos_closedir(void *d) { f_closedir(d); }

int kos_readdir(void *d, char *name, int n, int *is_dir)
{
    FILINFO fi;

    if (f_readdir(d, &fi) != FR_OK || !fi.fname[0])
        return -1;
    snprintf(name, (size_t)n, "%s", fi.fname);
    *is_dir = !!(fi.fattrib & AM_DIR);
    return 0;
}

uint32_t kos_time(void)
{
    rtc_datetime_t t;

    if (rtc_get(&t))
        return 0;
    return epoch(t.year, t.month, t.day, t.hour, t.min, t.sec);
}

void *kos_arena(size_t *size)
{
    *size = kit.arena_size;
    return kit.arena;
}

/* ---- the game's thread and its screen ------------------------------------------- */
static screen_t game;

static void finished(void *arg)
{
    k.running = false;
    ui_screen_mode(&game, 0);
    if (k.held) {                       /* an error stays up with nothing running */
        k.held = false;
        cpufreq_release();
    }
    if (k.on_stack && !kit.error[0]) {
        if (kit_game.rom_exts)
            kit_menu_after_game();      /* the list again, in a fresh app */
        /* quit: back to where it was opened. What is over it (the panel) goes too,
         * or the panel would go and the game's screen stay */
        for (int i = 0; i < 12 && k.on_stack; i++)
            ui_pop();
    } else {
        ui_redraw();                    /* the error, with Back */
    }
}

/* The game's memory is the app's (what an app's thread allocates is the
 * app's): the arena goes when the app is unloaded; the frame it frees. */
static void game_main(void *arg)
{
    int code = 1;
    size_t free_ = heap_free(), want;

    want = free_ > kit_game.arena_reserve + kit_game.arena_min ? free_ - kit_game.arena_reserve
                                                               : kit_game.arena_min;
    for (; want >= kit_game.arena_min && !(kit.arena = malloc(want)); want -= 64 * 1024)
        ;
    if (kit.arena) {
        kit.arena_size = want;
        klog("%s: %s, %lu KB for the game, %lu KB left", kit_game.log_name, k.dir,
             (unsigned long)(want / 1024), (unsigned long)(heap_free() / 1024));
        code = kit_run(k.argc, k.argv);
    } else {
        snprintf(kit.error, sizeof(kit.error), "Not enough memory (%lu KB free).",
                 (unsigned long)(heap_free() / 1024));
    }
    kit_audio_stop();
    mutex_lock(&k.frame_lock, WAIT_FOREVER);
    k.frame = k.ready = NULL;           /* no more frames (the LCD's goes on being
                                         * read until the UI sends another: freed
                                         * with the app) */
    mutex_unlock(&k.frame_lock);
    if (code && !kit.error[0])
        snprintf(kit.error, sizeof(kit.error), "%s stopped (%d).", app_info.name, code);
    {
        const uint32_t *low = k.thread->stack;
        size_t unused = 0;

        while (unused < k.thread->stack_size / 4 && low[unused] == 0x57a57a57)
            unused++;
        klog("%s: its heap never below %lu KB free", kit_game.log_name,
             (unsigned long)(kit_heap_low() / 1024));
        klog("%s: stack %lu of %lu KB, OS heap never below %lu KB", kit_game.log_name,
             (unsigned long)((k.thread->stack_size - unused * 4) / 1024),
             (unsigned long)(k.thread->stack_size / 1024), (unsigned long)(heap_low / 1024));
    }
    klog("%s: ended (%d)%s%s", kit_game.log_name, code, kit.error[0] ? ": " : "", kit.error);
    while (!ui_call(finished, NULL, 2000))
        ;
}

static void wrap_text(const char *p, int y)
{
    char line[32];

    while (*p && y < UI_BOTTOM - 10) {
        int n = 0, last = 0;

        while (p[n] && n < (int)sizeof(line) - 1) {
            line[n] = p[n];
            line[n + 1] = 0;
            if (gfx_text_width(line, FONT_SMALL, 1) > LCD_WIDTH - 12)
                break;
            if (p[n] == ' ')
                last = n;
            n++;
        }
        if (p[n] && last)
            n = last + 1;
        memcpy(line, p, (size_t)n);
        line[n] = 0;
        gfx_text_centered(y, line, C_TEXT, FONT_SMALL, 1);
        y += 11;
        p += n;
    }
}

static void game_draw(screen_t *s)
{
    if (k.running && !kit.frames) {     /* full screen already: loading */
        gfx_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, C_BG);
        gfx_text_centered(LCD_HEIGHT / 2 - 12, app_info.name, C_ACCENT, FONT_LARGE, 1);
        gfx_text_centered(LCD_HEIGHT / 2 + 6, "Loading...", C_DIM, FONT_SMALL, 1);
        return;
    }
    if (k.running) {
        if (mutex_lock(&k.frame_lock, 0) != WAIT_OK) {
            if (ui_present(k.shown))    /* a frame is being made: it says so when
                                         * done; the keys do not wait */
                return;
            mutex_lock(&k.frame_lock, WAIT_FOREVER);    /* under a dialog */
        }
        if (!k.frame) {                 /* the game has ended */
            gfx_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
        } else {
            uint16_t *px = k.ready ? k.ready : k.shown;

            if (!ui_present(px)) {      /* under a dialog: as a screen does */
                if (px)
                    gfx_blit(0, 0, LCD_WIDTH, LCD_HEIGHT, px);
            } else if (k.ready) {       /* the LCD's now: the other to draw in */
                uint16_t *other = (uint16_t *)k.frames + (k.ready == (uint16_t *)k.frames ? LCD_WIDTH * LCD_HEIGHT : 0);

                k.shown = k.ready;
                k.frame = other;
                k.ready = NULL;
            }
        }
        mutex_unlock(&k.frame_lock);
        return;
    }
    gfx_text_centered(UI_TOP + 20, app_info.name, C_ACCENT, FONT_LARGE, 1);
    wrap_text(kit.error[0] ? kit.error : "Starting...", UI_TOP + 44);
}

/* An emulator has no quit dialog of its own: END asks here, the game
 * waiting (kit_present*). Yes: it writes its battery save and ends, and
 * the list comes back. */
static void leave_draw(screen_t *s)
{
    gfx_text_centered(UI_TOP + 24, "Leave the game?", C_TEXT, FONT_SMALL, 1);
    gfx_text_centered(UI_TOP + 44, "(a battery save is kept)", C_DIM, FONT_SMALL, 1);
}

static void leave_key(screen_t *s, const key_event_t *ev)
{
    if (!ev->down)
        return;
    if (ev->code == KEY_END || ev->code == KEY_LSOFT || ev->code == KEY_CENTER)
        k.quit_asked = true;
    else if (ev->code != KEY_RSOFT)
        return;
    k.paused = false;
    ui_pop();
}

static screen_t leave_screen = {
    .title = "Leave?", .left = "Leave", .right = "Play on",
    .draw = leave_draw, .key = leave_key,
};

static void game_key(screen_t *s, const key_event_t *ev)
{
    if (k.running && kit_game.rom_exts && ev->code == KEY_END) {
        if (ev->down && !k.quit_asked) {
            k.paused = true;
            ui_push(&leave_screen);
            ui_screen_mode(&leave_screen, UI_MODE_RAW_KEYS | UI_MODE_AWAKE);
        }
        return;
    }
    if (k.running) {
        unsigned in = k.key_in;

        if (in - k.key_out < ARRAY_SIZE(k.keys)) {
            k.keys[in % ARRAY_SIZE(k.keys)].code = ev->code;
            k.keys[in % ARRAY_SIZE(k.keys)].down = ev->down;
            k.key_in = in + 1;
        }
        return;
    }
    if (ev->down && (ev->code == KEY_RSOFT || ev->code == KEY_CENTER))
        ui_pop();
}

static void game_enter(screen_t *s)
{
    k.on_stack = true;
    k.depth = ui_depth();
}

static void game_leave(screen_t *s)
{
    k.on_stack = false;
    k.quit_asked = true;                /* the game saves and ends */
}

static screen_t game = {
    .title = "", .right = "Back",
    .enter = game_enter, .leave = game_leave,
    .draw = game_draw, .key = game_key,
};

/* Start the game with the arguments and folder prepare() (or a menu of
 * the game's: kit_launch) set; the screen that shows it, or NULL. */
screen_t *kit_launch(void)
{
    if (k.running)
        return NULL;
    game.title = app_info.name;
    if (kit.error[0])
        return &game;                   /* why not: shown, Back */
    if (!k.argc)
        kit_arg(kit_game.log_name);
    if (!k.dir[0])
        kit_set_dir(app_dir() ? app_dir() : "/");
    k.frames = zalloc(2 * LCD_WIDTH * LCD_HEIGHT * 2 + 32);
    k.frames = k.frames ? (void *)(((uintptr_t)k.frames + 31) & ~(uintptr_t)31) : NULL;
    k.frame = k.frames;
    if (!k.frame) {
        snprintf(kit.error, sizeof(kit.error), "Not enough memory (%lu KB free).",
                 (unsigned long)(heap_free() / 1024));
        return &game;
    }
    last_rot = -1;                      /* the bars, the tables: anew */
    launched_ms = ticks();
    cpufreq_hold();
    k.held = true;
    k.running = true;
    k.thread = thread_create(kit_game.log_name, PRIO_LOW, kit_game.stack, game_main, NULL);
    if (!k.thread) {
        k.running = false;
        kit_fail("Could not start the game's thread.");
        return &game;
    }
    ui_screen_mode(&game, UI_MODE_FULL | UI_MODE_RAW_KEYS | UI_MODE_AWAKE);
    return &game;
}

static void reset(void)
{
    memset(&k, 0, sizeof(k));
    memset(&kit, 0, sizeof(kit));
    mutex_init(&k.frame_lock);
    k.view = kit_game.view;
}

screen_t *kit_start(void)
{
    if (k.thread || k.running)
        return NULL;                    /* still winding down */
    reset();
    if (!card_mounted()) {
        kit_fail("No memory card.");
        game.title = app_info.name;
        return &game;
    }
    if (kit_game.rom_exts)
        return kit_menu_start();        /* an emulator: the list, or the ROM */
    if (kit_game.prepare && !kit_game.prepare()) {
        game.title = app_info.name;
        if (!kit.error[0])
            kit_fail("The game could not start.");
        return &game;
    }
    return kit_launch();
}

void kit_stop(void)
{
    kit_audio_stop();
    kit_close_files();
    if (k.held)
        cpufreq_release();
    klog("%s: unloaded", kit_game.log_name);
}
