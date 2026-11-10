/* Quake on B310E OS: its system, video, sound and keys (sys.h, vid.h, sound.h) on the
 * game kit (os/ports/kit), where id's null drivers (sys_null.c, vid_null.c) are on a
 * PC. Quake itself is id's, its changes in prepare.py.
 *
 * Keys (held sideways, the D-pad turned with the screen):
 *   D-pad       move and turn; the menus
 *   Center      fire; select
 *   LSoft       jump; yes
 *   RSoft       the menu; back
 *   Dial        run (held)
 *   1, 3        strafe left, right
 *   4, 6        weapon before, after
 *   7, 9        look up, down; 0: centre the view
 *   5, 8        swim up, down
 *   *           the scores
 *   #           the console
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quakedef.h"
#include "d_local.h"

#include "kit.h"

size_t kit_heap_largest(void);

/* ---- the game ------------------------------------------------------------------------ */
#define HEAP_KEEP (96 * 1024)           /* for what it mallocs itself (files, the sound) */

static quakeparms_t parms;
qboolean isDedicated;                   /* (a phone is no server) */

int kit_main(int argc, char **argv)
{
    double last, now;

    parms.memsize = (int)(kit_heap_largest() - HEAP_KEEP) & ~0xfff;
    parms.membase = malloc(parms.memsize);
    if (!parms.membase)
        Sys_Error("no memory for its heap (%d KB)", parms.memsize / 1024);
    parms.basedir = ".";
    COM_InitArgv(argc, argv);
    parms.argc = com_argc;
    parms.argv = com_argv;
    Sys_Printf("Quake: %d KB for its heap\n", parms.memsize / 1024);
    Host_Init(&parms);
    /* weapons back and forth on the keypad (default.cfg has the next one only) */
    Cbuf_AddText("bind [ \"impulse 12\"\nbind ] \"impulse 10\"\n");
    last = Sys_FloatTime();
    for (;;) {
        now = Sys_FloatTime();
        Host_Frame(now - last);
        last = now;
    }
}

/* ---- files: stdio (the kit's, in the game's folder) ---------------------------------- */
#define MAX_HANDLES 10
static FILE *handles[MAX_HANDLES];

static int new_handle(void)
{
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!handles[i])
            return i;
    Sys_Error("out of file handles");
    return -1;
}

static int file_length(FILE *f)
{
    long pos = ftell(f), end;

    fseek(f, 0, SEEK_END);
    end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return (int)end;
}

int Sys_FileOpenRead(char *path, int *hndl)
{
    int i = new_handle();
    FILE *f = fopen(path, "rb");

    if (!f) {
        *hndl = -1;
        return -1;
    }
    handles[i] = f;
    *hndl = i;
    return file_length(f);
}

int Sys_FileOpenWrite(char *path)
{
    int i = new_handle();
    FILE *f = fopen(path, "wb");

    if (!f)
        Sys_Error("could not write %s", path);
    handles[i] = f;
    return i;
}

void Sys_FileClose(int handle)
{
    fclose(handles[handle]);
    handles[handle] = NULL;
}

void Sys_FileSeek(int handle, int position) { fseek(handles[handle], position, SEEK_SET); }
int Sys_FileRead(int handle, void *dest, int count) { return (int)fread(dest, 1, count, handles[handle]); }
int Sys_FileWrite(int handle, void *data, int count) { return (int)fwrite(data, 1, count, handles[handle]); }

int Sys_FileTime(char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f)
        return -1;
    fclose(f);
    return 1;
}

void Sys_mkdir(char *path) {}

/* ---- the system ---------------------------------------------------------------------- */
void Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length) {}
void Sys_DebugLog(char *file, char *fmt, ...) {}
char *Sys_ConsoleInput(void) { return NULL; }
void Sys_Sleep(void) { kit_sleep(1); }
void Sys_LowFPPrecision(void) {}
void Sys_HighFPPrecision(void) {}
void Sys_SetFPCW(void) {}

void Sys_Error(char *error, ...)
{
    char text[160];
    va_list ap;

    strcpy(text, "Error: ");
    va_start(ap, error);
    vsnprintf(text + 7, sizeof(text) - 7, error, ap);
    va_end(ap);
    kit_log(text);
    {   /* what its hunk held (a failed allocation's usual cause): not through the
         * console, which would draw */
        extern qboolean con_initialized;
        void Hunk_Print(qboolean all);
        static int once;

        con_initialized = false;
        if (!once++)
            Hunk_Print(false);
    }
    exit(1);
}

/* Its console's lines, to the log a line at a time */
void Sys_Printf(char *fmt, ...)
{
    static char line[160];
    static size_t used;
    char text[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p & 0x7f;     /* (its coloured text: high bit) */

        if (c == '\n' || used == sizeof(line) - 1) {
            line[used] = 0;
            if (used)
                kit_log(line);
            used = 0;
        }
        if (c >= ' ')
            line[used++] = (char)c;
    }
}

void Sys_Quit(void)
{
    Host_Shutdown();
    exit(0);
}

double Sys_FloatTime(void)
{
    static uint32_t base;

    if (!base)
        base = kit_ms() - 1;
    return (kit_ms() - base) / 1000.0;
}

/* ---- the keys ------------------------------------------------------------------------ */
struct quake_key { uint8_t game, menu; };

static const struct quake_key keys[128] = {
    [KIT_UP] = { K_UPARROW, K_UPARROW },
    [KIT_DOWN] = { K_DOWNARROW, K_DOWNARROW },
    [KIT_LEFT] = { K_LEFTARROW, K_LEFTARROW },
    [KIT_RIGHT] = { K_RIGHTARROW, K_RIGHTARROW },
    [KIT_CENTER] = { K_CTRL, K_ENTER },
    [KIT_LSOFT] = { K_SPACE, 'y' },
    [KIT_RSOFT] = { K_ESCAPE, K_ESCAPE },
    [KIT_DIAL] = { K_SHIFT, K_SHIFT },
    [KIT_END] = { K_ESCAPE, K_ESCAPE },
    [KIT_0 + 1] = { ',', '1' },
    [KIT_0 + 3] = { '.', '3' },
    [KIT_0 + 4] = { '[', '4' },
    [KIT_0 + 6] = { ']', '6' },
    [KIT_0 + 7] = { 'a', '7' },
    [KIT_0 + 9] = { 'z', '9' },
    [KIT_0 + 5] = { 'd', '5' },
    [KIT_0 + 8] = { 'c', '8' },
    [KIT_0 + 2] = { '2', '2' },
    [KIT_0] = { K_END, '0' },
    [KIT_STAR] = { K_TAB, K_BACKSPACE },
    [KIT_HASH] = { '`', '`' },
};

void Sys_SendKeyEvents(void)
{
    static uint8_t held[128];           /* what a key's press gave, for its release */
    int code;
    bool down;

    while (kit_key(&code, &down)) {
        if ((unsigned)code >= 128)
            continue;
        if (down)
            held[code] = key_dest == key_game ? keys[code].game : keys[code].menu;
        if (held[code])
            Key_Event(held[code], down);
    }
    if (kit_quit_asked()) {             /* the screen left, or END held: out */
        static bool leaving;

        if (!leaving) {
            leaving = true;
            Sys_Quit();
        }
    }
}

/* ---- video: 320 x 200, 8-bit; the 3D view drawn at 160 x 100 (VIEW_W, VIEW_H: the LCD
 * shows no more) into its own buffer and doubled into it (prepare.py); its surface cache
 * in its heap */
#define WIDTH 320
#define HEIGHT 200
#define SURFCACHE (96 * 1024)           /* (id's is 600 KB at 320 x 200) */

static byte vid_buffer[WIDTH * HEIGHT];
byte b310e_view[VIEW_W * VIEW_H];
static short zbuffer[VIEW_W * VIEW_H];
unsigned short d_8to16table[256];
unsigned d_8to24table[256];

void VID_SetPalette(unsigned char *palette) { kit_palette(palette, 256); }
void VID_ShiftPalette(unsigned char *palette) { kit_palette(palette, 256); }

void VID_Init(unsigned char *palette)
{
    vid.maxwarpwidth = vid.width = vid.conwidth = WIDTH;
    vid.maxwarpheight = vid.height = vid.conheight = HEIGHT;
    vid.aspect = 1.0;
    vid.numpages = 1;
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
    vid.buffer = vid.conbuffer = vid_buffer;
    vid.rowbytes = vid.conrowbytes = WIDTH;
    d_pzbuffer = zbuffer;
    D_InitCaches(Hunk_HighAllocName(SURFCACHE, "surfcache"), SURFCACHE);
    VID_SetPalette(palette);
}

void VID_Shutdown(void) {}

/* The view's rectangle (r_refdef.vrect, at half size) doubled into the screen's */
void B310E_ViewToScreen(void)
{
    const vrect_t *r = &r_refdef.vrect;

    for (int y = 0; y < r->height; y++) {
        const byte *src = b310e_view + (r->y + y) * VIEW_W + r->x;
        uint16_t *a = (uint16_t *)(vid_buffer + (2 * (r->y + y)) * WIDTH + 2 * r->x);
        uint16_t *b = (uint16_t *)((byte *)a + WIDTH);

        for (int x = 0; x < r->width; x++) {
            uint16_t two = src[x] | src[x] << 8;

            a[x] = two;
            b[x] = two;
        }
    }
}

void VID_Update(vrect_t *rects)
{
    kit_present8(vid_buffer, WIDTH, HEIGHT, WIDTH);
}

void D_BeginDirectRect(int x, int y, byte *pbitmap, int width, int height) {}
void D_EndDirectRect(int x, int y, int width, int height) {}

/* ---- sound: its DMA ring, emptied by the kit's sound thread -------------------------- */
#define RATE 11025
#define RING 8192                       /* samples (left and right each one): 0.37 s */

static dma_t dma;
static int16_t ring[RING];
static volatile unsigned played;        /* samples taken since the start */

static void render(int16_t *lr, unsigned frames)
{
    unsigned at = played;

    for (unsigned i = 0; i < frames * 2; i++)
        lr[i] = ring[(at + i) & (RING - 1)];
    played = at + frames * 2;
}

qboolean SNDDMA_Init(void)
{
    memset(&dma, 0, sizeof(dma));
    shm = &dma;
    shm->splitbuffer = 0;
    shm->samplebits = 16;
    shm->speed = RATE;
    shm->channels = 2;
    shm->samples = RING;
    shm->samplepos = 0;
    shm->submission_chunk = 1;
    shm->buffer = (unsigned char *)ring;
    played = 0;
    if (kit_audio_start(RATE, render)) {
        shm = NULL;
        return false;
    }
    return true;
}

int SNDDMA_GetDMAPos(void)
{
    shm->samplepos = played & (RING - 1);
    return shm->samplepos;
}

void SNDDMA_Submit(void) {}
void SNDDMA_Shutdown(void) { kit_audio_stop(); }
