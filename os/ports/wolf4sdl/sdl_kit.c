/* Wolf4SDL on B310E OS: sdl/SDL.h and sdl/SDL_mixer.h on the game kit.
 *
 * Video: every surface is 8-bit with a palette (the screen too, so
 * Wolf4SDL draws the screenBits == 8 way); the streaming "texture" the
 * screen is copied to is the kit's frame (kit_present8, the palette last
 * set). Keys: the phone's, as keyboard scancodes Wolf4SDL's controls know
 * (wolf_os.c's table). Sound: a mixer of 8 channels over the kit's sound
 * thread, the music hook first (the OPL), then the channels, then the
 * post-mix (the PC speaker).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"
#include "SDL_mixer.h"
#include "kit.h"

/* ---- the rest ------------------------------------------------------------------------ */
int SDL_Init(Uint32 flags) { (void)flags; return 0; }
void SDL_Quit(void) {}
const char *SDL_GetError(void) { return "no SDL"; }
void SDL_Delay(Uint32 ms) { kit_sleep(ms ? ms : 1); }
Uint32 SDL_GetTicks(void) { return kit_ms(); }
SDL_bool SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return SDL_TRUE; }

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, void *w)
{
    (void)title;
    (void)w;
    /* Quit()'s text: stderr's last line is shown when the game stops */
    fprintf(flags & SDL_MESSAGEBOX_ERROR ? stderr : stdout, "%s\n", message);
    return 0;
}

/* ---- video --------------------------------------------------------------------------- */
struct SDL_Window { int w, h; };
struct SDL_Renderer { int unused; };
struct SDL_Texture { int w, h; };
static struct SDL_Window the_window;
static struct SDL_Renderer the_renderer;

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    (void)title; (void)x; (void)y; (void)flags;
    the_window.w = w;
    the_window.h = h;
    return &the_window;
}

void SDL_DestroyWindow(SDL_Window *w) { (void)w; }
SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, int i, Uint32 f) { (void)w; (void)i; (void)f; return &the_renderer; }
void SDL_DestroyRenderer(SDL_Renderer *r) { (void)r; }
int SDL_SetRenderDrawBlendMode(SDL_Renderer *r, int m) { (void)r; (void)m; return 0; }

SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, Uint32 format, int access, int w, int h)
{
    SDL_Texture *t = malloc(sizeof(*t));

    (void)r; (void)format; (void)access;
    if (t) {
        t->w = w;
        t->h = h;
    }
    return t;
}

void SDL_DestroyTexture(SDL_Texture *t) { free(t); }

/* the screen's 8 bits (screenBits 8) to the LCD, the palette last set */
int SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *rect, const void *pixels, int pitch)
{
    (void)rect;
    kit_present8(pixels, t->w, t->h, pitch);
    return 0;
}

int SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t, const SDL_Rect *s, const SDL_Rect *d)
{
    (void)r; (void)t; (void)s; (void)d;
    return 0;
}

void SDL_RenderPresent(SDL_Renderer *r) { (void)r; }

SDL_bool SDL_PixelFormatEnumToMasks(Uint32 format, int *bpp, Uint32 *r, Uint32 *g, Uint32 *b,
                                    Uint32 *a)
{
    (void)format;
    *bpp = 8;                           /* the screen is 8-bit too */
    *r = *g = *b = *a = 0;
    return SDL_TRUE;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth, Uint32 r, Uint32 g,
                                  Uint32 b, Uint32 a)
{
    SDL_Surface *s = calloc(1, sizeof(*s));

    (void)flags; (void)depth; (void)r; (void)g; (void)b; (void)a;
    if (!s)
        return NULL;
    s->w = w;
    s->h = h;
    s->pitch = (w + 3) & ~3;
    s->pixels = calloc(1, (size_t)s->pitch * h);
    if (!s->pixels) {
        free(s);
        return NULL;
    }
    s->pal.ncolors = 256;
    s->pal.colors = s->colors;
    s->fmt.palette = &s->pal;
    s->fmt.BitsPerPixel = 8;
    s->fmt.BytesPerPixel = 1;
    s->format = &s->fmt;
    return s;
}

void SDL_FreeSurface(SDL_Surface *s)
{
    if (s) {
        free(s->pixels);
        free(s);
    }
}

int SDL_SetPaletteColors(SDL_Palette *p, const SDL_Color *colors, int first, int n)
{
    uint8_t rgb[256 * 3];

    if (first < 0 || first + n > 256)
        return -1;
    memcpy(p->colors + first, colors, (size_t)n * sizeof(SDL_Color));
    for (int i = 0; i < 256; i++) {
        rgb[3 * i] = p->colors[i].r;
        rgb[3 * i + 1] = p->colors[i].g;
        rgb[3 * i + 2] = p->colors[i].b;
    }
    kit_palette(rgb, 256);              /* the screen's and the buffer's are one */
    return 0;
}

int SDL_LockSurface(SDL_Surface *s) { (void)s; return 0; }
void SDL_UnlockSurface(SDL_Surface *s) { (void)s; }

int SDL_FillRect(SDL_Surface *s, const SDL_Rect *rect, Uint32 color)
{
    SDL_Rect all = { 0, 0, s->w, s->h };

    if (!rect)
        rect = &all;
    for (int y = rect->y; y < rect->y + rect->h && y < s->h; y++)
        if (y >= 0)
            memset((Uint8 *)s->pixels + y * s->pitch + rect->x, (int)color, (size_t)rect->w);
    return 0;
}

int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *sr, SDL_Surface *dst, SDL_Rect *dr)
{
    (void)sr;
    (void)dr;                           /* Wolf4SDL blits whole surfaces */
    for (int y = 0; y < src->h && y < dst->h; y++)
        memcpy((Uint8 *)dst->pixels + y * dst->pitch, (Uint8 *)src->pixels + y * src->pitch,
               (size_t)(src->w < dst->w ? src->w : dst->w));
    return 0;
}

Uint32 SDL_MapRGBA(const SDL_PixelFormat *f, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    (void)f; (void)a;
    return (Uint32)r << 16 | (Uint32)g << 8 | b;   /* (the 32-bit path only) */
}

int SDL_SaveBMP(SDL_Surface *s, const char *file) { (void)s; (void)file; return -1; }
int SDL_ShowCursor(int toggle) { (void)toggle; return 0; }

/* ---- keys ------------------------------------------------------------------------------ */
/* the phone's key to Wolf3D's (wolf_os.c): up to two scancodes, and whether
 * it toggles (Dial: run) */
extern const SDL_Scancode wolf_keys[128][2];
extern const Uint8 wolf_toggles[128];

#define QUEUE 32
static SDL_Event queue[QUEUE];
static unsigned q_in, q_out;
static Uint8 toggled[128];
static int quit_sent;

static void push(Uint32 type, SDL_Scancode sc)
{
    SDL_Event *e;

    if (q_in - q_out >= QUEUE || sc == SDL_SCANCODE_UNKNOWN)
        return;
    e = &queue[q_in++ % QUEUE];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->key.keysym.scancode = sc;
    e->key.keysym.sym = (SDL_Keycode)sc;
    e->key.state = type == SDL_KEYDOWN;
}

static void pump(void)
{
    int code;
    bool down;

    if (kit_quit_asked() && !quit_sent) {
        SDL_Event *e = &queue[q_in++ % QUEUE];

        memset(e, 0, sizeof(*e));
        e->type = SDL_QUIT;             /* Wolf4SDL quits (and saves its config) */
        quit_sent = 1;
    }
    while (kit_key(&code, &down)) {
        code &= 127;
        if (wolf_toggles[code]) {       /* on, off */
            if (!down)
                continue;
            toggled[code] ^= 1;
            down = toggled[code];
        }
        for (int i = 0; i < 2; i++)
            push(down ? SDL_KEYDOWN : SDL_KEYUP, wolf_keys[code][i]);
    }
}

int SDL_PollEvent(SDL_Event *e)
{
    pump();
    if (q_out == q_in)
        return 0;
    if (e)
        *e = queue[q_out % QUEUE];
    q_out++;
    return 1;
}

int SDL_WaitEvent(SDL_Event *e)
{
    for (;;) {
        pump();
        if (q_out != q_in) {
            if (e) {
                *e = queue[q_out % QUEUE];
                q_out++;
            }
            return 1;
        }
        kit_sleep(5);
    }
}

Uint8 SDL_EventState(Uint32 type, int state) { (void)type; (void)state; return 0; }
int SDL_GetModState(void) { return KMOD_NUM; }      /* the keypad is digits */
Uint32 SDL_GetMouseState(int *x, int *y) { if (x) *x = 0; if (y) *y = 0; return 0; }
Uint32 SDL_GetRelativeMouseState(int *x, int *y) { if (x) *x = 0; if (y) *y = 0; return 0; }
int SDL_SetRelativeMouseMode(int on) { (void)on; return 0; }
void SDL_SetWindowGrab(SDL_Window *w, int on) { (void)w; (void)on; }
void SDL_WarpMouseInWindow(SDL_Window *w, int x, int y) { (void)w; (void)x; (void)y; }

int SDL_NumJoysticks(void) { return 0; }
SDL_Joystick *SDL_JoystickOpen(int index) { (void)index; return NULL; }
void SDL_JoystickClose(SDL_Joystick *j) { (void)j; }
void SDL_JoystickUpdate(void) {}
int SDL_JoystickNumButtons(SDL_Joystick *j) { (void)j; return 0; }
int SDL_JoystickNumHats(SDL_Joystick *j) { (void)j; return 0; }
Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int a) { (void)j; (void)a; return 0; }
Uint8 SDL_JoystickGetHat(SDL_Joystick *j, int h) { (void)j; (void)h; return 0; }
Uint8 SDL_JoystickGetButton(SDL_Joystick *j, int b) { (void)j; (void)b; return 0; }

/* ---- the mixer: 8 channels of 8-bit samples, panned 0..255 ---------------------------- */
/* Wolf4SDL's digitised sounds are 7042 Hz (ORIGSAMPLERATE): kept at that,
 * stepped through at the output's rate as they are mixed */
#define KEEP_RATE 7042

static struct {
    const Sint8 *data;                  /* NULL: free */
    Uint32 n, pos;                      /* samples; pos in 16.16 */
    int left, right;                    /* 0..255 */
    int group;
    Uint32 started;
} ch[MIX_CHANNELS];

static int rate;
static void (*music)(void *, Uint8 *, int);
static void *music_arg;
static void (*postmix)(void *, Uint8 *, int);
static void *postmix_arg;
static void (*finished)(int);
static Uint32 play_count;

static void render(int16_t *lr, unsigned frames)
{
    Uint32 step = rate ? (Uint32)(((uint64_t)KEEP_RATE << 16) / (unsigned)rate) : 0;

    memset(lr, 0, frames * 4);
    if (music)
        music(music_arg, (Uint8 *)lr, (int)frames * 4);
    for (int c = 0; c < MIX_CHANNELS; c++) {
        const Sint8 *d;
        Uint32 pos, n;
        int l, r;
        unsigned s = kit_irq_save();

        d = ch[c].data;
        pos = ch[c].pos;
        n = ch[c].n;
        l = ch[c].left;
        r = ch[c].right;
        kit_irq_restore(s);
        if (!d)
            continue;
        for (unsigned i = 0; i < frames && pos >> 16 < n; i++, pos += step) {
            int v = d[pos >> 16];
            int a = lr[2 * i] + v * l, b = lr[2 * i + 1] + v * r;   /* << 8 >> 8 */

            lr[2 * i] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
            lr[2 * i + 1] = (int16_t)(b > 32767 ? 32767 : b < -32768 ? -32768 : b);
        }
        s = kit_irq_save();
        if (ch[c].data == d) {          /* not replaced meanwhile */
            ch[c].pos = pos;
            if (pos >> 16 >= n)
                ch[c].data = NULL;
        }
        kit_irq_restore(s);
        if (pos >> 16 >= n && finished)
            finished(c);
    }
    if (postmix)
        postmix(postmix_arg, (Uint8 *)lr, (int)frames * 4);
}

int Mix_OpenAudioDevice(int frequency, Uint16 format, int channels, int chunksize,
                        const char *device, int allowed_changes)
{
    (void)format; (void)channels; (void)chunksize; (void)device; (void)allowed_changes;
    rate = frequency;
    for (int c = 0; c < MIX_CHANNELS; c++) {
        ch[c].left = ch[c].right = 255;
        ch[c].group = -1;
    }
    return kit_audio_start((unsigned)rate, render);
}

int Mix_QuerySpec(int *frequency, Uint16 *format, int *channels)
{
    if (frequency)
        *frequency = rate;
    if (format)
        *format = AUDIO_S16;
    if (channels)
        *channels = 2;
    return 1;
}

const char *Mix_GetError(void) { return "no sound"; }
int Mix_ReserveChannels(int num) { return num; }

int Mix_GroupChannels(int from, int to, int tag)
{
    for (int c = from; c <= to && c < MIX_CHANNELS; c++)
        ch[c].group = tag;
    return to - from + 1;
}

int Mix_GroupAvailable(int tag)
{
    for (int c = 0; c < MIX_CHANNELS; c++)
        if ((tag == -1 || ch[c].group == tag) && !ch[c].data)
            return c;
    return -1;
}

int Mix_GroupOldest(int tag)
{
    int best = -1;

    for (int c = 0; c < MIX_CHANNELS; c++)
        if ((tag == -1 || ch[c].group == tag) && ch[c].data &&
            (best < 0 || (int32_t)(ch[c].started - ch[best].started) < 0))
            best = c;
    return best;
}

void Mix_HookMusic(void (*fn)(void *, Uint8 *, int), void *arg)
{
    unsigned s = kit_irq_save();

    music = fn;
    music_arg = arg;
    kit_irq_restore(s);
}

void Mix_SetPostMix(void (*fn)(void *, Uint8 *, int), void *arg)
{
    unsigned s = kit_irq_save();

    postmix = fn;
    postmix_arg = arg;
    kit_irq_restore(s);
}

void Mix_ChannelFinished(void (*fn)(int)) { finished = fn; }

SDL_RWops *SDL_RWFromMem(void *mem, int size)
{
    SDL_RWops *rw = malloc(sizeof(*rw));

    if (rw) {
        rw->data = mem;
        rw->size = (size_t)size;
    }
    return rw;
}

/* Wolf4SDL's own WAV: 16-bit mono at the output's rate, made from its
 * 8-bit 7042 Hz sounds; kept as they were (8-bit, KEEP_RATE): Wolf4SDL keeps
 * every sound, and the whole VSWAP besides, so a sixth of the memory */
Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc)
{
    Mix_Chunk *c = NULL;
    size_t at = 12;

    while (src && at + 8 <= src->size) {
        Uint32 len = src->data[at + 4] | src->data[at + 5] << 8 |
                     (Uint32)src->data[at + 6] << 16 | (Uint32)src->data[at + 7] << 24;

        if (!memcmp(src->data + at, "data", 4)) {
            if (len > src->size - at - 8)
                len = (Uint32)(src->size - at - 8);
            Uint32 have = len / 2, keep = (Uint32)((uint64_t)have * KEEP_RATE / (unsigned)rate);

            if ((c = malloc(sizeof(*c))) && (c->abuf = malloc(keep ? keep : 1))) {
                for (Uint32 i = 0; i < keep; i++) {     /* the high byte of each */
                    Uint32 j = (Uint32)((uint64_t)i * rate / KEEP_RATE);

                    c->abuf[i] = src->data[at + 8 + 2 * (j < have ? j : have - 1) + 1];
                }
                c->alen = keep;
                c->allocated = 1;
                c->volume = 128;
            } else {
                free(c);
                c = NULL;
            }
            break;
        }
        at += 8 + len + (len & 1);
    }
    if (freesrc)
        free(src);
    return c;
}

void Mix_FreeChunk(Mix_Chunk *c)
{
    if (c) {
        for (int i = 0; i < MIX_CHANNELS; i++)
            if (ch[i].data == (const Sint8 *)c->abuf)
                Mix_HaltChannel(i);
        free(c->abuf);
        free(c);
    }
}

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops)
{
    unsigned s;

    (void)loops;
    if (channel < 0)
        channel = Mix_GroupAvailable(-1);
    if (channel < 0 || channel >= MIX_CHANNELS || !chunk)
        return -1;
    s = kit_irq_save();
    ch[channel].data = (const Sint8 *)chunk->abuf;
    ch[channel].n = chunk->alen;
    ch[channel].pos = 0;
    ch[channel].started = ++play_count;
    kit_irq_restore(s);
    return channel;
}

int Mix_HaltChannel(int channel)
{
    unsigned s = kit_irq_save();

    for (int c = 0; c < MIX_CHANNELS; c++)
        if (channel < 0 || c == channel)
            ch[c].data = NULL;
    kit_irq_restore(s);
    return 0;
}

int Mix_SetPanning(int channel, Uint8 left, Uint8 right)
{
    if (channel < 0 || channel >= MIX_CHANNELS)
        return 0;
    ch[channel].left = left;
    ch[channel].right = right;
    return 1;
}

int Mix_Playing(int channel)
{
    return channel >= 0 && channel < MIX_CHANNELS && ch[channel].data != NULL;
}
