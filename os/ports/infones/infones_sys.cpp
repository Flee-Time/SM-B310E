/* InfoNES (the NES) on B310E OS: its system interface (InfoNES_System.h)
 * on the game kit (os/ports/kit). InfoNES itself is upstream's, unchanged
 * (Apache-2.0, with prepare.py's changes). Its lines (256, of 240) go
 * one by one to the LCD as the ROM's view says, less the 8 lines top and bottom a TV
 * hid when "Edges" is "Cropped"; the five sound channels are mixed into a
 * ring the kit's sound thread empties (44100 Hz: InfoNES_pAPU.h's), which
 * also sets the game's pace; a battery-backed game's SRAM is <rom>.srm.
 *
 * Keys (the D-pad turned with the screen):
 *   D-pad             the D-pad
 *   Center, 7, 8, 9   A (sideways: to the right of B)
 *   4, 5, 6           B
 *   RSoft, Dial, #    Start
 *   LSoft, *, 0       Select
 *   END               asks to leave (the kit); the SRAM is written
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_pAPU.h"

#include "kit.h"

/* the NES's 64 colours: InfoNES's Linux table in RGB565, the lowest bit of
 * green clear: InfoNES's flag on the backdrop's colour (prepare.py) */
WORD NesPalette[64] = {
    0x738e, 0x20d1, 0x0015, 0x4013, 0x880e, 0xa802, 0xa000, 0x7840,
    0x4140, 0x0200, 0x0280, 0x01c2, 0x19cb, 0x0000, 0x0000, 0x0000,
    0xbdd7, 0x039d, 0x21dd, 0x801e, 0xb817, 0xe00b, 0xd940, 0xca41,
    0x8b80, 0x0480, 0x0540, 0x0487, 0x0411, 0x0000, 0x0000, 0x0000,
    0xffdf, 0x3ddf, 0x5c9f, 0x445f, 0xf3df, 0xfb96, 0xfb8c, 0xfcc7,
    0xf5c7, 0x8682, 0x4ec9, 0x5fd3, 0x075b, 0x0000, 0x0000, 0x0000,
    0xffdf, 0xaf1f, 0xc69f, 0xd65f, 0xfe1f, 0xfe1b, 0xfdd6, 0xfed5,
    0xff14, 0xe7d4, 0xaf97, 0xb7d9, 0x9fde, 0x0000, 0x0000, 0x0000,
};

extern int InfoNES_LineFirst, InfoNES_LineCount, InfoNES_ScanEnd;

enum { TV_AUTO, TV_NTSC, TV_PAL };
#define FRAME_US_NTSC 16667             /* 735 samples at 44100 Hz */
#define FRAME_US_PAL 20000              /* 882 */

static bool crop = true;
static int tv = TV_AUTO;
static int frame_us = FRAME_US_NTSC;
static char sram_file[160];
static bool quit;

/* ---- the game: kit_main(argv[0], "--crop" or "--whole", the ROM) ----------------- */
static void sram_path(const char *rom)
{
    const char *dot = strrchr(rom, '.');
    int n = dot ? (int)(dot - rom) : (int)strlen(rom);

    snprintf(sram_file, sizeof(sram_file), "%.*s.srm", n, rom);
}

static void sram_load(void)
{
    FILE *f;

    if (!ROM_SRAM || !(f = fopen(sram_file, "rb")))
        return;
    fread(SRAM, 1, SRAM_SIZE, f);
    fclose(f);
}

static void sram_save(void)
{
    FILE *f;

    if (!ROM_SRAM || !(f = fopen(sram_file, "wb")))
        return;
    fwrite(SRAM, 1, SRAM_SIZE, f);
    fclose(f);
}

/* A ROM's own word for it: most iNES headers have none, so its name, or the
 * ROM's options (TV) */
static bool says_pal(const char *rom)
{
    static const char *const names[] = { "(E)", "(Europe)", "(PAL)", "(EU)" };
    bool clean = true;

    if ((NesHeader.byInfo2 & 0x0c) == 0x08)     /* NES 2.0: byte 12 says, 1 is PAL */
        return (NesHeader.byReserve[4] & 3) == 1;
    for (int i = 2; i < 8; i++)         /* (bytes 10-15: junk in old dumps) */
        clean = clean && !NesHeader.byReserve[i];
    if (clean && NesHeader.byReserve[1] & 1)
        return true;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strstr(rom, names[i]))
            return true;
    return false;
}

extern "C" int kit_main(int argc, char **argv)
{
    const char *rom = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--whole"))
            crop = false;
        else if (!strcmp(argv[i], "--crop"))
            crop = true;
        else if (!strcmp(argv[i], "--pal"))
            tv = TV_PAL;
        else if (!strcmp(argv[i], "--ntsc"))
            tv = TV_NTSC;
        else
            rom = argv[i];
    }
    if (!rom) {
        fprintf(stderr, "No ROM.\n");
        return 1;
    }
    if (crop) {                         /* not drawn at all (prepare.py) */
        InfoNES_LineFirst = 8;
        InfoNES_LineCount = NES_DISP_HEIGHT - 16;
    }
    sram_path(rom);
    if (InfoNES_Load(rom) < 0) {
        fprintf(stderr, "%s is not a NES ROM InfoNES can play.\n", rom);
        return 1;
    }
    sram_load();
    InfoNES_Main();                     /* until END (or the screen leaves) */
    sram_save();
    InfoNES_ReleaseRom();
    return 0;
}

int InfoNES_Menu()
{
    return quit ? -1 : 0;
}

int InfoNES_ReadRom(const char *name)
{
    FILE *f = fopen(name, "rb");

    if (!f)
        return -1;
    if (fread(&NesHeader, sizeof(NesHeader), 1, f) != 1 ||
        memcmp(NesHeader.byID, "NES\x1a", 4) != 0) {
        fclose(f);
        return -1;
    }
    if (tv == TV_PAL || (tv == TV_AUTO && says_pal(name))) {
        InfoNES_ScanEnd = 312;          /* here: InfoNES_Load sets up its sound next */
        frame_us = FRAME_US_PAL;
        kit_log("PAL: 50 frames a second");
    }
    memset(SRAM, 0, SRAM_SIZE);
    if (NesHeader.byInfo1 & 4)          /* a trainer: at 0x7000 */
        fread(&SRAM[0x1000], 512, 1, f);
    ROM = (BYTE *)malloc(NesHeader.byRomSize * 0x4000);
    if (!ROM || fread(ROM, 0x4000, NesHeader.byRomSize, f) != NesHeader.byRomSize) {
        fclose(f);
        return -1;
    }
    if (NesHeader.byVRomSize > 0) {
        VROM = (BYTE *)malloc(NesHeader.byVRomSize * 0x2000);
        if (!VROM || fread(VROM, 0x2000, NesHeader.byVRomSize, f) != NesHeader.byVRomSize) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

void InfoNES_ReleaseRom()
{
    free(ROM);
    ROM = NULL;
    free(VROM);
    VROM = NULL;
}

void *InfoNES_MemoryCopy(void *dest, const void *src, int count) { return memcpy(dest, src, count); }
void *InfoNES_MemorySet(void *dest, int c, int count) { return memset(dest, c, count); }
void InfoNES_DebugPrint(char *msg) { kit_log(msg); }

void InfoNES_MessageBox(char *fmt, ...)
{
    char text[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    kit_log(text);
}

/* ---- the frame ---------------------------------------------------------------------- */
static void on_time(void);

/* the log's numbers: where a frame's time goes, in ms (a whole frame's over
 * 10 s of them, so 1 ms clocks will do) */
static uint32_t t_mark, t_drawn, t_undrawn, t_sound, t_waited, n_drawn, n_undrawn;
static bool drawn, in_frame;

/* (prepare.py: InfoNES has no frame of its own) */
void InfoNES_LineDone(int line, WORD *px)
{
    const int top = crop ? 8 : 0;

    if (!in_frame) {
        kit_begin565(NES_DISP_WIDTH, NES_DISP_HEIGHT - 2 * top);
        in_frame = true;
    }
    if (line >= top && line < NES_DISP_HEIGHT - top)
        kit_line565(line - top, px);
}

void InfoNES_LoadFrame()
{
    on_time();
    if (in_frame) {
        kit_end565();
        in_frame = false;
    }
    drawn = true;
}

/* ---- the pads ----------------------------------------------------------------------- */
static DWORD pad;

static DWORD pad_bit(int code)
{
    switch (code) {
    case KIT_CENTER:
    case KIT_0 + 7:
    case KIT_0 + 8:
    case KIT_0 + 9:  return 0x01;       /* A */
    case KIT_0 + 4:
    case KIT_0 + 5:
    case KIT_0 + 6:  return 0x02;       /* B */
    case KIT_LSOFT:
    case KIT_STAR:
    case KIT_0:      return 0x04;       /* Select */
    case KIT_RSOFT:
    case KIT_DIAL:
    case KIT_HASH:   return 0x08;       /* Start */
    case KIT_UP:     return 0x10;
    case KIT_DOWN:   return 0x20;
    case KIT_LEFT:   return 0x40;
    case KIT_RIGHT:  return 0x80;
    }
    return 0;
}

void InfoNES_PadState(DWORD *pad1, DWORD *pad2, DWORD *sys)
{
    int code;
    bool down;

    while (kit_key(&code, &down)) {
        if (down)
            pad |= pad_bit(code);
        else
            pad &= ~pad_bit(code);
    }
    if (kit_quit_asked())
        quit = true;
    *pad1 = pad;
    *pad2 = 0;
    *sys = quit ? PAD_SYS_QUIT : 0;
}

/* ---- the sound: five channels into a ring; the ring sets the pace ---------------- */
#define RING 4096                       /* frames: 93 ms at 44100 Hz */
#define AHEAD 2048
#define MAX_SKIP 3

static int16_t ring[RING];
static volatile unsigned ring_in, ring_out;
static bool sound_on;

static void render(int16_t *lr, unsigned frames)
{
    unsigned i, have = ring_in - ring_out;

    for (i = 0; i < frames && i < have; i++)
        lr[2 * i] = lr[2 * i + 1] = ring[(ring_out + i) % RING];
    ring_out += i;
    for (; i < frames; i++)
        lr[2 * i] = lr[2 * i + 1] = 0;
}

void InfoNES_SoundInit(void) {}

int InfoNES_SoundOpen(int samples_per_sync, int sample_rate)
{
    (void)samples_per_sync;
    ring_in = ring_out = 0;
    sound_on = kit_audio_start((unsigned)sample_rate, render) == 0;
    return sound_on;
}

void InfoNES_SoundClose(void)
{
    kit_audio_stop();
    sound_on = false;
}

/* A frame drawn: is it on time? Late by more than half a frame, draw fewer;
 * in time, draw more, and if there is time left wait for it (shown when due,
 * by the clock: the codec's 6 ms blocks are too coarse to scroll by). The
 * clock is nudged to stay with the sound, the ring's level showing how. */
static void on_time(void)
{
    static uint32_t due_us, due_ms;
    static int level = AHEAD * 8;
    uint32_t now = kit_ms();
    int period = frame_us, early;

    if (sound_on) {                     /* 1 sample is 22.7 us; a quarter of it */
        level += (int)(ring_in - ring_out) - level / 8;
        period += (level / 8 - AHEAD) * 6;
    }
    due_us += (uint32_t)period * (FrameSkip + 1u);
    due_ms += due_us / 1000;
    due_us %= 1000;
    early = (int32_t)(due_ms - now);
    if (early < -100 || early > 100) {  /* far behind (or just started) */
        due_ms = now;
    } else if (early < -8) {
        if (FrameSkip < MAX_SKIP)
            FrameSkip++;
    } else if (FrameSkip) {
        FrameSkip--;
    } else if (early > 0) {
        kit_sleep((uint32_t)early);
        t_waited += (uint32_t)early;
    }
}

/* Every frame, after the sound's made: the log's numbers. (InfoNES_pAPUVsync
 * is InfoNES's, in C++'s name: --wrap in infones.mk.) */
extern "C" void __real__Z17InfoNES_pAPUVsyncv(void);

extern "C" void __wrap__Z17InfoNES_pAPUVsyncv(void)
{
    uint32_t t = kit_ms();

    if (drawn) {
        t_drawn += t - t_mark;
        n_drawn++;
    } else {
        t_undrawn += t - t_mark;
        n_undrawn++;
    }
    drawn = false;
    __real__Z17InfoNES_pAPUVsyncv();
    t_mark = kit_ms();
    t_sound += t_mark - t;
    if (n_drawn + n_undrawn >= 600) {
        char text[120];

        snprintf(text, sizeof(text), "%lu frames drawn at %lu ms, %lu not at %lu ms; "
                 "sound %lu, waited %lu ms a frame",
                 (unsigned long)n_drawn,
                 (unsigned long)(n_drawn ? (t_drawn - t_waited) / n_drawn : 0),
                 (unsigned long)n_undrawn,
                 (unsigned long)(n_undrawn ? t_undrawn / n_undrawn : 0),
                 (unsigned long)(t_sound / 600),
                 (unsigned long)(t_waited / 600));
        kit_log(text);
        t_drawn = t_undrawn = t_sound = t_waited = n_drawn = n_undrawn = 0;
    }
}

/* The channels are all above 0, silence 0 (noise to InfoNES's 15, so it
 * stays a hiss under the rest). A slow high-pass, as on the NES, takes out
 * the level they stand on, so a sound (and a gap in it) does not move the
 * speaker by itself. */
void InfoNES_SoundOutput(int samples, BYTE *w1, BYTE *w2, BYTE *w3, BYTE *w4, BYTE *w5)
{
    static int dc;
    unsigned in = ring_in;

    if (!sound_on)
        return;
    while (RING - (in - ring_out) < (unsigned)samples)
        kit_sleep(2);
    for (int i = 0; i < samples; i++) {
        int v = (w1[i] + w2[i] + w3[i] + w4[i] + w5[i]) * 64;

        dc += (v - dc) >> 8;
        v -= dc;
        if (v > 32767)
            v = 32767;
        else if (v < -32768)
            v = -32768;
        ring[in++ & (RING - 1)] = (int16_t)v;
    }
    ring_in = in;
}

void InfoNES_Wait() {}                  /* (a line's: on_time() is the frame's) */
