/* The SNES on B310E OS: a libretro frontend on the game kit (os/ports/kit) for
 * snes9x2002 (its core and libretro.c, unchanged but for prepare.py's). The frame
 * (256 x 224, RGB565) goes to the LCD as the view says, the sound (stereo, 32 kHz)
 * through a ring the kit's sound thread empties, whose fill drives the core's own
 * frameskip; the keypad is the joypad. The cartridge's battery RAM is kept beside the
 * ROM (.srm).
 *
 * Keys (held sideways, the D-pad turned with the screen):
 *   D-pad         the D-pad
 *   Center, 5     B
 *   LSoft, 6      A
 *   4, *          Y
 *   8, #          X
 *   1, 7          L
 *   3, 9          R
 *   RSoft         Start
 *   Dial, 0       Select
 *   END           quit (the battery RAM is written)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "snes9x.h"
#include "memmap.h"

#include "kit.h"

unsigned s9x_rom_alloc;                 /* the ROM's buffer (memmap.c, prepare.py) */
unsigned s9x_rom_paged;                 /* a ROM in pages: its size (memmap.c), else 0 */
extern uint8 *HDMAMemPointers[8];

/* ---- a ROM bigger than the memory: in pages from the card ------------------------------ */
/* What the core takes besides the ROM (tile caches, RAM, screens) is about CORE_NEEDS: a
 * ROM that does not fit beside it is read in pages as the game reaches them. A page is
 * a bank's ROM (32 KB, 64 KB for HiROM), so the CPU's PCBase, which spans a bank, stays
 * in one. The ROM's start (HEAD: its header, its vectors) is read whole; each of Map's
 * other ROM blocks is MAP_PAGED until its page is in a slot (getset.h calls S9xPageIn).
 * The least recently used page gives way, never the one the CPU or an HDMA reads from.
 * Every AGE frames all pages are marked out again (their data stays): the next touch
 * maps them back and so says they are used */
#define CORE_NEEDS (1600 * 1024)
#define HEAD 0x10000
#define AGE 30
#define KEEP_FREE (96 * 1024)           /* for the core's own mallocs after the load */

static FILE *rom_file;
static long rom_skip;                   /* a copier's header */
static unsigned page_size;
static int32_t vrom[MEMMAP_NUM_BLOCKS]; /* a paged block's ROM offset, else -1 */
static struct slot { uint8 *data; int page; uint32_t used; } slots[96];
static int nslots;
static uint32_t page_tick, page_reads, page_runs;

static void slot_map(const struct slot *s, bool in)
{
    for (int b = 0; b < MEMMAP_NUM_BLOCKS; b++)
        if (vrom[b] >= 0 && vrom[b] / (int)page_size == s->page)
            Memory.Map[b] = in ? s->data + (vrom[b] - s->page * (int)page_size)
                                     - ((b << MEMMAP_SHIFT) & 0xffff)
                               : (uint8 *)MAP_PAGED;
}

static bool slot_busy(const struct slot *s)
{
    const uint8 *lo = s->data, *hi = s->data + page_size;

    if (CPU.PC >= lo && CPU.PC < hi)
        return true;
    for (int d = 0; d < 8; d++)
        if (HDMAMemPointers[d] >= lo && HDMAMemPointers[d] <= hi)
            return true;
    return false;
}

uint8 *S9xPageIn(uint32 Address)
{
    int b = (Address >> MEMMAP_SHIFT) & MEMMAP_MASK;
    struct slot *s = NULL;
    int page;

    if (vrom[b] < 0)                    /* (not a paged block: as it is) */
        return Memory.Map[b];
    page = vrom[b] / (int)page_size;
    for (int i = 0; i < nslots && !s; i++)
        if (slots[i].page == page)
            s = &slots[i];
    if (!s) {                           /* the least used that nothing reads from */
        for (int i = 0; i < nslots; i++)
            if (!slot_busy(&slots[i]) && (!s || slots[i].used < s->used))
                s = &slots[i];
        if (!s) {
            kit_log("snes9x: every ROM page is in use");
            exit(1);
        }
        if (s->page >= 0)
            slot_map(s, false);
        fseek(rom_file, rom_skip + (long)page * (long)page_size, SEEK_SET);
        fread(s->data, 1, page_size, rom_file);
        s->page = page;
        page_reads++;
    }
    s->used = ++page_tick;
    slot_map(s, true);
    return Memory.Map[b];
}

static void pages_setup(void)
{
    for (int b = 0; b < MEMMAP_NUM_BLOCKS; b++) {
        uint8 *m = Memory.Map[b];
        int32_t off;

        vrom[b] = -1;
        if (!Memory.BlockIsROM[b] || m < (uint8 *)MAP_LAST)
            continue;
        off = (int32_t)(m - Memory.ROM) + ((b << MEMMAP_SHIFT) & 0xffff);
        if (off < HEAD || off >= (int32_t)s9x_rom_paged)
            continue;
        vrom[b] = off;
        Memory.Map[b] = (uint8 *)MAP_PAGED;
    }
    page_size = Memory.HiROM ? 0x10000 : 0x8000;
    while (nslots < (int)(sizeof(slots) / sizeof(slots[0])) &&
           kit_heap_largest() > page_size + KEEP_FREE &&
           (slots[nslots].data = malloc(page_size)) != NULL)
        slots[nslots++].page = -1;
}

static void pages_age(void)
{
    for (int i = 0; i < nslots; i++)
        if (slots[i].page >= 0)
            slot_map(&slots[i], false);
}

/* ---- the keys -------------------------------------------------------------------------- */
static const signed char pad_of[128] = {
    [KIT_UP] = RETRO_DEVICE_ID_JOYPAD_UP + 1,
    [KIT_DOWN] = RETRO_DEVICE_ID_JOYPAD_DOWN + 1,
    [KIT_LEFT] = RETRO_DEVICE_ID_JOYPAD_LEFT + 1,
    [KIT_RIGHT] = RETRO_DEVICE_ID_JOYPAD_RIGHT + 1,
    [KIT_CENTER] = RETRO_DEVICE_ID_JOYPAD_B + 1,
    [KIT_0 + 5] = RETRO_DEVICE_ID_JOYPAD_B + 1,
    [KIT_LSOFT] = RETRO_DEVICE_ID_JOYPAD_A + 1,
    [KIT_0 + 6] = RETRO_DEVICE_ID_JOYPAD_A + 1,
    [KIT_0 + 4] = RETRO_DEVICE_ID_JOYPAD_Y + 1,
    [KIT_STAR] = RETRO_DEVICE_ID_JOYPAD_Y + 1,
    [KIT_0 + 8] = RETRO_DEVICE_ID_JOYPAD_X + 1,
    [KIT_HASH] = RETRO_DEVICE_ID_JOYPAD_X + 1,
    [KIT_0 + 1] = RETRO_DEVICE_ID_JOYPAD_L + 1,
    [KIT_0 + 7] = RETRO_DEVICE_ID_JOYPAD_L + 1,
    [KIT_0 + 3] = RETRO_DEVICE_ID_JOYPAD_R + 1,
    [KIT_0 + 9] = RETRO_DEVICE_ID_JOYPAD_R + 1,
    [KIT_RSOFT] = RETRO_DEVICE_ID_JOYPAD_START + 1,
    [KIT_DIAL] = RETRO_DEVICE_ID_JOYPAD_SELECT + 1,
    [KIT_0] = RETRO_DEVICE_ID_JOYPAD_SELECT + 1,
};
static uint8_t held[16];                /* how many keys hold each button */

static void input_poll(void)
{
    int code;
    bool down;

    while (kit_key(&code, &down))
        if ((unsigned)code < 128 && pad_of[code]) {
            uint8_t *n = &held[pad_of[code] - 1];

            if (down)
                (*n)++;
            else if (*n)
                (*n)--;
        }
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    return port == 0 && device == RETRO_DEVICE_JOYPAD && id < 16 && held[id];
}

/* ---- the frame ------------------------------------------------------------------------- */
static uint32_t frames;

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (data)
        kit_present565(data, (int)width, (int)height, (int)(pitch / 2));
    frames++;
}

/* ---- the sound: a ring of stereo frames ------------------------------------------------ */
#define RATE 32000                      /* (the core's is 32040: near enough) */
#define RING 4096                       /* frames: 0.13 s */

static int16_t ring[RING * 2];
static volatile unsigned ring_in, ring_out;
static bool underrun;

static void render(int16_t *lr, unsigned n)
{
    unsigned out = ring_out;

    for (unsigned i = 0; i < n; i++) {
        if (out == ring_in) {           /* nothing more: silence */
            memset(lr + 2 * i, 0, (n - i) * 4);
            underrun = true;
            break;
        }
        lr[2 * i] = ring[2 * (out & (RING - 1))];
        lr[2 * i + 1] = ring[2 * (out & (RING - 1)) + 1];
        out++;
    }
    ring_out = out;
}

static size_t audio_batch(const int16_t *data, size_t n)
{
    unsigned in = ring_in;

    for (size_t i = 0; i < n && in - ring_out < RING; i++, in++) {
        ring[2 * (in & (RING - 1))] = data[2 * i];
        ring[2 * (in & (RING - 1)) + 1] = data[2 * i + 1];
    }
    ring_in = in;
    return n;
}

static void audio_sample(int16_t left, int16_t right)
{
    int16_t lr[2] = { left, right };

    audio_batch(lr, 1);
}

static retro_audio_buffer_status_callback_t buffer_status;

/* ---- what the core asks of the frontend ------------------------------------------------ */
static bool environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = data;

        if (!strcmp(var->key, "snes9x2002_frameskip")) {
            var->value = "auto";        /* a frame left out when the sound runs dry */
            return true;
        }
        return false;
    }
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
        const struct retro_audio_buffer_status_callback *cb = data;

        buffer_status = cb ? cb->callback : NULL;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_OVERSCAN:
        *(bool *)data = false;
        return true;
    default:
        return false;
    }
}

/* ---- the battery RAM, beside the ROM --------------------------------------------------- */
static char srm_path[160];

static void srm(bool save)
{
    void *ram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    FILE *f;

    if (!ram || !size)
        return;
    if (!(f = fopen(srm_path, save ? "wb" : "rb")))
        return;
    if (save)
        fwrite(ram, 1, size, f);
    else
        fread(ram, 1, size, f);
    fclose(f);
}

/* ---- the game -------------------------------------------------------------------------- */
int kit_main(int argc, char **argv)
{
    struct retro_game_info game = { 0 };
    char *dot;
    FILE *f;
    long size;
    uint32_t start, ran = 0;

    if (argc < 2) {
        kit_log("snes9x: no ROM");
        return 1;
    }
    if (!(f = fopen(argv[1], "rb"))) {
        kit_log("snes9x: the ROM does not open");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    retro_set_environment(environment);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);

    if ((size & 0x7fff) == 512) {       /* a copier's header: not the game's */
        rom_skip = 512;
        size -= 512;
    }
    if (kit_heap_largest() < (size_t)size + 0x8200 + CORE_NEEDS)
        s9x_rom_paged = (unsigned)size;
    s9x_rom_alloc = (s9x_rom_paged ? HEAD : (unsigned)size) + 0x200 + 0x8000;
    retro_init();
    if (!Memory.ROM) {
        char text[96];

        snprintf(text, sizeof(text), "snes9x: no room for the ROM (%ld KB)", size / 1024);
        kit_log(text);
        fclose(f);
        return 1;
    }
    Settings.ForceNoHeader = TRUE;      /* (taken off here) */
    if (s9x_rom_paged)
        Settings.ForceNotInterleaved = TRUE;
    /* straight into the core's buffer: one copy (its loader reads it in place) */
    game.path = argv[1];
    game.data = Memory.ROM;
    fseek(f, rom_skip, SEEK_SET);
    game.size = fread(Memory.ROM, 1, s9x_rom_paged ? HEAD : (size_t)size, f);
    if (s9x_rom_paged)
        rom_file = f;
    else
        fclose(f);
    if (!retro_load_game(&game)) {
        kit_log("snes9x: the ROM did not load");
        return 1;
    }
    if (s9x_rom_paged) {
        char text[112];

        if (Settings.SA1 || Settings.SuperFX || Settings.SDD1 || Settings.C4) {
            kit_log("snes9x: a game with its own chip has to fit whole");
            return 1;
        }
        pages_setup();
        snprintf(text, sizeof(text), "snes9x: %ld KB ROM in %u KB pages, %d of them in memory",
                 size / 1024, page_size / 1024, nslots);
        kit_log(text);
    }
    snprintf(srm_path, sizeof(srm_path), "%s", argv[1]);
    if ((dot = strrchr(srm_path, '.')) != NULL)
        *dot = 0;
    strncat(srm_path, ".srm", sizeof(srm_path) - strlen(srm_path) - 1);
    srm(false);

    kit_audio_start(RATE, render);
    start = kit_ms();
    while (!kit_quit_asked()) {
        if (buffer_status) {
            unsigned queued = ring_in - ring_out;

            buffer_status(true, queued * 100 / RING, underrun);
            underrun = false;
        }
        retro_run();
        if (s9x_rom_paged && ++page_runs % AGE == 0)
            pages_age();
        /* no faster than the SNES: its 60 frames a second (counted from start:
         * 1000 / 60 in whole ms would be 62.5) */
        int32_t early = (int32_t)(start + ++ran * 50 / 3 - kit_ms());

        if (early > 0)
            kit_sleep((uint32_t)early);
        else if (early < -100) {        /* behind: from here on */
            start = kit_ms();
            ran = 0;
        }
    }
    kit_audio_stop();
    srm(true);
    if (s9x_rom_paged) {
        char text[64];

        snprintf(text, sizeof(text), "snes9x: %u ROM pages read", (unsigned)page_reads);
        kit_log(text);
    }
    retro_unload_game();
    retro_deinit();
    return 0;
}
