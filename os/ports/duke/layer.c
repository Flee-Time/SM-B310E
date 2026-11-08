/* Duke Nukem 3D on B310E OS: the Build engine's platform layer (what its
 * sdlayer2.c is for SDL) on the game kit: a 320 x 200 8-bit frame, the keys as
 * the scancodes its MACT library knows, the timer, and what the desktop has
 * that a phone does not.
 *
 * Keys (held sideways, the D-pad turned with the screen):
 *   D-pad       move and turn; the menus
 *   Center      fire; select
 *   LSoft       open, use; yes
 *   RSoft       the menu; back
 *   Dial        run on / off
 *   5, 8        jump, crouch
 *   1, 3        strafe left, right
 *   4, 6        weapon before, after
 *   7, 9        item before, after
 *   0           use the item
 *   *           quick kick
 *   #           the map
 *   END         quit (asks; LSoft: yes)
 *
 * This file, the game kit it runs on and what they link with: see
 * os/ports/duke/README.md for the licences.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "duke3d.h"
#include "baselayer_priv.h"
#include "keyboard.h"
#include "kit.h"

void setvlinebpl(int dabpl);         /* (a.h, the engine's own) */
void fx_poll(void);                     /* (duke_sound.c) */
unsigned kit_io_ms(void);               /* (kit_sys.c) */
void keep_effects(void);                /* (premap.c, from prepare.py) */
void gameexit(const char *t);           /* (game.c) */

#define WIDTH 320
#define HEIGHT 200

/* ---- the game ------------------------------------------------------------------------ */
int kit_main(int argc, char **argv)
{
    _buildargc = argc;
    _buildargv = (const char **)argv;
    baselayer_init();
    return app_main(argc, (char const *const *)argv);
}

int initsystem(void) { return 0; }
void uninitsystem(void) {}
void initputs(const char *s) { (void)s; }

void debugprintf(const char *f, ...) { (void)f; }

void wm_setapptitle(const char *name) { (void)name; }
void wm_setwindowtitle(const char *name) { (void)name; }
int wm_idle(void *p) { (void)p; return 0; }

int wm_msgbox(const char *name, const char *fmt, ...)
{
    char text[120];
    va_list ap;

    (void)name;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    kit_log(text);
    return 0;
}

int wm_ynbox(const char *name, const char *fmt, ...)
{
    (void)name;
    (void)fmt;
    return 0;                           /* no one to ask: no */
}

int wm_filechooser(const char *initialdir, const char *initialfile, const char *type, int foropen,
                   char **choice)
{
    (void)initialdir; (void)initialfile; (void)type; (void)foropen; (void)choice;
    return 0;
}

int startwin_open(void) { return 0; }
int startwin_close(void) { return 0; }
int startwin_puts(const char *s) { (void)s; return 0; }
int startwin_settitle(const char *s) { (void)s; return 0; }
int startwin_idle(void *p) { (void)p; return 0; }
int startwin_run(struct startwin_settings *s) { (void)s; return STARTWIN_RUN; }

/* The engine's tile cache takes 60% of this (loadpics). As much of the arena as
 * can be: what the game mallocs after it (the music, the frame) needs about 360 KB;
 * its sounds are in the cache. compat.c's asks the host. */
#define OUTSIDE_CACHE (360 * 1024)

size_t __wrap_Bgetsysmemsize(void)
{
    size_t have = kit_heap_largest();

    return have > 2 * OUTSIDE_CACHE ? (have - OUTSIDE_CACHE) / 60 * 100 : have;
}

/* The game keeps to its folder: the kit opens everything from there */
int chdir(const char *path) { (void)path; return 0; }

char *getcwd(char *buf, size_t size)
{
    if (size < 2)
        return NULL;
    strcpy(buf, ".");
    return buf;
}

/* (duke_compat.h: a directory's entries are looked at from the game's folder) */
int fstatat(int fd, const char *name, struct stat *st, int flags)
{
    (void)fd; (void)flags;
    return stat(name, st);
}

/* ---- the frame ----------------------------------------------------------------------- */
static uint8_t *frame;

void getvalidmodes(void)
{
    validmode[0].xdim = WIDTH;
    validmode[0].ydim = HEIGHT;
    validmode[0].bpp = 8;
    validmode[0].fs = 1;
    validmodecnt = 1;
}

int setvideomode(int x, int y, int c, int fs)
{
    if (!frame && !(frame = malloc(WIDTH * HEIGHT + 8)))
        return -1;
    frameplace = (intptr_t)frame;
    bytesperline = WIDTH;
    imageSize = WIDTH * HEIGHT;
    numpages = 1;
    setvlinebpl(WIDTH);
    for (int i = 0, j = 0; i <= HEIGHT; i++, j += WIDTH)
        ylookup[i] = j;
    (void)x; (void)y;
    xres = WIDTH;
    yres = HEIGHT;
    bpp = c;
    fullscreen = fs;
    modechange = 1;
    videomodereset = 0;
    setpalettefade(palfadergb.r, palfadergb.g, palfadergb.b, palfadedelta);
    return 0;
}

void resetvideomode(void) {}
void begindrawing(void) {}
void enddrawing(void) {}

/* Its menus and logos draw as fast as they can: no faster than the LCD shows */
#define FRAME_MS 16

/* What a frame was made of, for the log: a dip's walls, sprites, card, or the rest
 * (the game's logic, the status bar) */
static uint32_t rooms_ms, masks_ms;

void __real_drawrooms(int x, int y, int z, short ang, int horiz, short sect);
void __real_drawmasks(void);

void __wrap_drawrooms(int x, int y, int z, short ang, int horiz, short sect)
{
    uint32_t t = kit_ms();

    __real_drawrooms(x, y, z, ang, horiz, sect);
    rooms_ms += kit_ms() - t;
}

void __wrap_drawmasks(void)
{
    uint32_t t = kit_ms();

    __real_drawmasks();
    masks_ms += kit_ms() - t;
}

static void frame_parts(uint32_t now, uint32_t present_ms)
{
    static uint32_t at, told, last_io, worst, w_rooms, w_masks, w_io, w_present;
    uint32_t io = kit_io_ms();

    if (at && now - at > worst) {
        worst = now - at;
        w_rooms = rooms_ms;
        w_masks = masks_ms;
        w_io = io - last_io;
        w_present = present_ms;
    }
    at = now;
    last_io = io;
    rooms_ms = masks_ms = 0;
    if (now - told >= 10000) {
        char text[96];

        snprintf(text, sizeof(text), "worst frame %u ms: walls %u, sprites %u, of those card %u; present %u",
                 (unsigned)worst, (unsigned)w_rooms, (unsigned)w_masks, (unsigned)w_io, (unsigned)w_present);
        kit_log(text);
        told = now;
        worst = 0;
    }
}

void showframe(void)
{
    static uint32_t last, kept, present_ms;
    uint32_t now = kit_ms();

    frame_parts(now, present_ms);
    if (now - last < FRAME_MS) {
        kit_sleep(FRAME_MS - (now - last));
        now = kit_ms();
    }
    last = now;
    if (now - kept >= 1000) {
        kept = now;
        keep_effects();
    }
    if (frame)
        kit_present8(frame, WIDTH, HEIGHT, WIDTH);
    present_ms = kit_ms() - now;
}

int setpalette(int start, int num, unsigned char *dapal)
{
    static uint8_t rgb[256 * 3];

    (void)start; (void)num; (void)dapal;
    for (int i = 0; i < 256; i++) {
        rgb[3 * i] = curpalettefaded[i].r;
        rgb[3 * i + 1] = curpalettefaded[i].g;
        rgb[3 * i + 2] = curpalettefaded[i].b;
    }
    kit_palette(rgb, 256);
    return 0;
}

int setgamma(float gamma) { (void)gamma; return 0; }

/* ---- the timer ----------------------------------------------------------------------- */
static int tics_per_second;
static void (*tick_callback)(void);
static uint32_t last_ms;
static uint64_t sub;

int inittimer(int tickspersecond, void (*callback)(void))
{
    if (!tics_per_second) {
        tics_per_second = tickspersecond;
        tick_callback = callback;
        last_ms = kit_ms();
    }
    return 0;
}

void uninittimer(void) { tics_per_second = 0; }

void sampletimer(void)
{
    uint32_t now = kit_ms();
    int n;

    if (!tics_per_second)
        return;
    sub += (uint64_t)(now - last_ms) * (unsigned)tics_per_second;
    last_ms = now;
    n = (int)(sub / 1000);
    sub %= 1000;
    totalclock += n;
    while (n-- > 0 && tick_callback)
        tick_callback();
}

unsigned int getticks(void) { return kit_ms(); }
unsigned int getusecticks(void) { return kit_ms() * 1000u; }
int gettimerfreq(void) { return 1000; }

/* ---- the keys ------------------------------------------------------------------------ */
/* A key's scancode in the game, and in the menus if another */
static const struct { uint8_t game, menu, menu2; } keys[128] = {
    [KIT_UP] = { sc_UpArrow, 0, 0 },
    [KIT_DOWN] = { sc_DownArrow, 0, 0 },
    [KIT_LEFT] = { sc_LeftArrow, 0, 0 },
    [KIT_RIGHT] = { sc_RightArrow, 0, 0 },
    [KIT_CENTER] = { sc_LeftControl, sc_Return, 0 },
    [KIT_LSOFT] = { sc_Space, sc_Return, sc_Y },
    [KIT_RSOFT] = { sc_Escape, sc_Escape, sc_N },
    [KIT_DIAL] = { sc_CapsLock, 0, 0 },
    [KIT_END] = { sc_F10, sc_F10, 0 },
    [KIT_0 + 1] = { sc_Comma, 0, 0 },
    [KIT_0 + 3] = { sc_Period, 0, 0 },
    [KIT_0 + 4] = { sc_SemiColon, 0, 0 },
    [KIT_0 + 6] = { sc_Quote, 0, 0 },
    [KIT_0 + 5] = { sc_A, 0, 0 },
    [KIT_0 + 8] = { sc_Z, 0, 0 },
    [KIT_0 + 7] = { sc_OpenBracket, 0, 0 },
    [KIT_0 + 9] = { sc_CloseBracket, 0, 0 },
    [KIT_0] = { sc_Return, 0, 0 },
    [KIT_STAR] = { sc_Tilde, 0, 0 },
    [KIT_HASH] = { sc_Tab, 0, 0 },
};

int initinput(void)
{
    inputdevices = 1;                   /* the keyboard */
    return 0;
}

void uninitinput(void) {}
const char *getkeyname(int num) { (void)num; return ""; }
const char *getjoyname(int what, int num) { (void)what; (void)num; return ""; }
int initmouse(void) { return 0; }
void uninitmouse(void) {}
void grabmouse(int a) { (void)a; }
void readmousexy(int *x, int *y) { *x = *y = 0; }
void readmousebstatus(int *b) { *b = 0; }
void releaseallbuttons(void) {}

static void scan(uint8_t sc, bool down)
{
    if (!sc)
        return;
    if (down && !keystatus[sc]) {
        keyfifo[keyfifoend] = sc;
        keyfifo[(keyfifoend + 1) & (KEYFIFOSIZ - 1)] = 1;
        keyfifoend = (keyfifoend + 2) & (KEYFIFOSIZ - 1);
    }
    keystatus[sc] = down;
}

int handleevents(void)
{
    static uint8_t held[128][2];        /* what a key's press gave, for its release */
    int code;
    bool down;

    while (kit_key(&code, &down)) {
        if ((unsigned)code >= 128)
            continue;
        if (down) {
            bool menu = (ps[myconnectindex].gm & MODE_MENU) != 0;

            held[code][0] = menu && keys[code].menu ? keys[code].menu : keys[code].game;
            held[code][1] = menu ? keys[code].menu2 : 0;
        }
        scan(held[code][0], down);
        scan(held[code][1], down);
    }
    sampletimer();
    fx_poll();
    if (kit_quit_asked()) {             /* the screen left, or END held: its settings, and out */
        static bool leaving;

        if (!leaving) {
            leaving = true;
            gameexit("");
        }
    }
    return 0;
}
