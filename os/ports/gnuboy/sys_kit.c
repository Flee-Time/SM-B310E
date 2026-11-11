/* gnuboy on B310E OS: its system interface (sys.h) on the game kit
 * (os/ports/kit): the frame (160 x 144, RGB565) to the LCD as the ROM's
 * view says, the sound (8-bit stereo at 22050 Hz, as gnuboy mixes it)
 * through a ring the kit's sound thread empties, the keypad as the keys
 * gnuboy's default bindings know. gnuboy itself is upstream's, unchanged.
 *
 * Keys (the D-pad turned with the screen):
 *   D-pad         the D-pad
 *   Center, 5     A
 *   LSoft, 4      B
 *   Dial, 6       Start
 *   0, 8          Select
 *   7, 9          save state, load state (slot 0)
 *   END           quit (the game's battery save is written)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "defs.h"
#include "fb.h"
#include "input.h"
#include "pcm.h"
#include "rc.h"
#include "sys.h"
#include "rtc.h"
#include <time.h>

#include "kit.h"

struct fb fb;
struct pcm pcm;

int gnuboy_main(int argc, char **argv);
int __real_loader_init(char *s);
char *loader_get_error(void);

/* gnuboy's main() plays on without a cartridge when the ROM does not load
 * (a black screen): the link wraps loader_init, so its error is shown */
int __wrap_loader_init(char *s)
{
    int r = __real_loader_init(s);

    if (r) {
        fprintf(stderr, "%s", loader_get_error() ? loader_get_error() : "The ROM did not load.\n");
        exit(1);
    }
    return r;
}

/* The cartridge clock (.rtc) caught up with the time it was off in one step: gnuboy's
 * own rtc_load_internal ticked it once per 1/60 s of it, in an int (a save from 2015,
 * when the phone's clock was not set: eleven years of ticks, the app at 99% on
 * "Loading..." for ever). The link wraps rtc_load_internal (gnuboy.mk). */
void __wrap_rtc_load_internal(FILE *f)
{
    long saved = 0;
    int64_t ticks;

    if (fscanf(f, "%d %d %d %d %d %d %d\n%ld\n", &rtc.carry, &rtc.stop, &rtc.d, &rtc.h, &rtc.m,
               &rtc.s, &rtc.t, &saved) < 7)
        return;
    rtc.t %= 60;
    rtc.s %= 60;
    rtc.m %= 60;
    rtc.h %= 24;
    rtc.d %= 365;
    rtc.stop &= 1;
    rtc.carry &= 1;
    if (!saved || rtc.stop || time(0) <= saved)
        return;
    ticks = rtc.t + 60 * (rtc.s + 60 * (rtc.m + 60 * (rtc.h + 24 * (int64_t)rtc.d))) +
            ((int64_t)time(0) - saved) * 60;
    rtc.t = (int)(ticks % 60);
    ticks /= 60;
    rtc.s = (int)(ticks % 60);
    ticks /= 60;
    rtc.m = (int)(ticks % 60);
    ticks /= 60;
    rtc.h = (int)(ticks % 24);
    ticks /= 24;
    if (ticks >= 365)                   /* the day counter ran over, as rtc_tick has it */
        rtc.carry = 1;
    rtc.d = (int)(ticks % 365);
}

int kit_main(int argc, char **argv)
{
    return gnuboy_main(argc, argv);
}

/* ---- video ----------------------------------------------------------------------- */
static uint16_t frame[160 * 144];

rcvar_t vid_exports[] = { RCV_END };
rcvar_t joy_exports[] = { RCV_END };
rcvar_t pcm_exports[] = { RCV_END };

void vid_preinit(void) {}

void vid_init(void)
{
    memset(&fb, 0, sizeof(fb));
    fb.w = 160;
    fb.h = 144;
    fb.pelsize = 2;
    fb.pitch = 160 * 2;
    fb.ptr = (byte *)frame;
    fb.cc[0].r = 3;                     /* RGB565: what each channel loses, */
    fb.cc[0].l = 11;                    /* where it goes */
    fb.cc[1].r = 2;
    fb.cc[1].l = 5;
    fb.cc[2].r = 3;
    fb.cc[2].l = 0;
    fb.enabled = 1;
    fb.dirty = 1;
    fb.delegate_scaling = 1;            /* the kit scales */
}

void vid_close(void) { fb.enabled = 0; }
void vid_settitle(char *title) { (void)title; }
void vid_setpal(int i, int r, int g, int b) { (void)i; (void)r; (void)g; (void)b; }
void vid_begin(void) {}

void vid_end(void)
{
    if (fb.enabled)
        kit_present565(frame, 160, 144, 160);
}

/* ---- sound: gnuboy's 8-bit stereo into a ring, 16-bit out of it -------------------- */
#define RATE 22050
#define RING 4096                       /* frames: 186 ms */
#define AHEAD 2048                      /* gnuboy waits when this far ahead */

static byte mix[1024];                  /* gnuboy's buffer: 512 stereo frames */
static int16_t ring[RING][2];
static volatile unsigned ring_in, ring_out;
static int sound_on;

static void render(int16_t *lr, unsigned frames)
{
    unsigned i, have = ring_in - ring_out;

    for (i = 0; i < frames && i < have; i++) {
        lr[2 * i] = ring[(ring_out + i) % RING][0];
        lr[2 * i + 1] = ring[(ring_out + i) % RING][1];
    }
    ring_out += i;
    for (; i < frames; i++)
        lr[2 * i] = lr[2 * i + 1] = 0;  /* behind: silence */
}

void pcm_init(void)
{
    pcm.hz = RATE;
    pcm.stereo = 1;
    pcm.buf = mix;
    pcm.len = sizeof(mix);
    pcm.pos = 0;
    ring_in = ring_out = 0;
    sound_on = kit_audio_start(RATE, render) == 0;
}

void pcm_close(void)
{
    kit_audio_stop();
    sound_on = 0;
    memset(&pcm, 0, sizeof(pcm));
}

void pcm_pause(int dopause) { (void)dopause; }

/* gnuboy's buffer is full (or a frame is done): into the ring. Ahead of
 * the sound by AHEAD frames, it waits: the sound sets the game's pace.
 * 1: it waited (gnuboy then keeps no time of its own). */
int pcm_submit(void)
{
    int waited = 0;

    if (!sound_on) {
        pcm.pos = 0;
        return 0;
    }
    for (int i = 0; i + 1 < pcm.pos; i += 2) {
        while (ring_in - ring_out >= RING) {
            kit_sleep(2);
            waited = 1;
        }
        ring[ring_in % RING][0] = (int16_t)((pcm.buf[i] - 128) << 8);
        ring[ring_in % RING][1] = (int16_t)((pcm.buf[i + 1] - 128) << 8);
        ring_in++;
    }
    pcm.pos = 0;
    while (ring_in - ring_out > AHEAD) {
        kit_sleep(2);
        waited = 1;
    }
    return waited;
}

/* ---- keys ------------------------------------------------------------------------ */
static int gb_key(int code)
{
    switch (code) {
    case KIT_UP:     return K_UP;
    case KIT_DOWN:   return K_DOWN;
    case KIT_LEFT:   return K_LEFT;
    case KIT_RIGHT:  return K_RIGHT;
    /* sideways the keypad's columns stand as rows would: 7 8 9 right of
     * 4 5 6, as A is right of B */
    case KIT_CENTER:
    case KIT_0 + 7:
    case KIT_0 + 8:
    case KIT_0 + 9:  return 'd';        /* +a */
    case KIT_0 + 4:
    case KIT_0 + 5:
    case KIT_0 + 6:  return 's';        /* +b */
    case KIT_RSOFT:
    case KIT_DIAL:
    case KIT_HASH:   return K_ENTER;    /* +start */
    case KIT_LSOFT:
    case KIT_STAR:
    case KIT_0:      return K_SPACE;    /* +select */
    case KIT_0 + 1:  return K_INS;      /* savestate */
    case KIT_0 + 3:  return K_DEL;      /* loadstate */
    }
    return 0;
}

void ev_poll(int wait)
{
    int code, k;
    bool down;
    event_t ev;

    (void)wait;
    if (kit_quit_asked())
        exit(0);                        /* the battery save is written at exit */
    while (kit_key(&code, &down)) {
        if (!(k = gb_key(code)))
            continue;
        memset(&ev, 0, sizeof(ev));
        ev.type = down ? EV_PRESS : EV_RELEASE;
        ev.code = k;
        ev_postevent(&ev);
    }
}

void joy_init(void) {}
void joy_poll(void) {}
void joy_close(void) {}
void kb_init(void) {}
void kb_poll(void) {}
void kb_close(void) {}

/* ---- the rest of sys.h ---------------------------------------------------------------- */
/* gnuboy's clock, in its struct timeval: the kit's milliseconds */
static struct timeval timer;

static void now_tv(struct timeval *tv)
{
    uint32_t ms = kit_ms();

    tv->tv_sec = (time_t)(ms / 1000);
    tv->tv_usec = (suseconds_t)(ms % 1000) * 1000;
}

void *sys_timer(void)
{
    now_tv(&timer);
    return &timer;
}

int sys_elapsed(struct timeval *prev)
{
    struct timeval now;
    int us;

    now_tv(&now);
    us = (int)((now.tv_sec - prev->tv_sec) * 1000000 + (now.tv_usec - prev->tv_usec));
    *prev = now;
    return us;
}

void sys_sleep(int us)
{
    if (us >= 1000)
        kit_sleep((uint32_t)us / 1000);
}

void sys_checkdir(char *path, int wr)
{
    struct stat st;

    (void)wr;
    if (stat(path, &st))
        mkdir(path, 0777);
}

void sys_initpath(void)
{
    char *dot = ".";                    /* the ROM's folder (the kit's) */

    rc_setvar("rcpath", 1, &dot);
    rc_setvar("savedir", 1, &dot);
}

void sys_sanitize(char *s) { (void)s; }
