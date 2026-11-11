/* The SNES on B310E OS: a libretro frontend on the game kit (os/ports/kit) for
 * snes9x2002 (its core and libretro.c, unchanged but for prepare.py's). The frame
 * (256 x 224, RGB565) goes to the LCD as the view says, the sound (stereo, 32 kHz)
 * through a ring the kit's sound thread empties: what is queued for the speaker paces
 * the game and says which frames are drawn; the keypad is the joypad. The cartridge's battery RAM is kept beside the
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
#include <malloc.h>

#include "libretro.h"
#include "snes9x.h"
#include "memmap.h"
#include "dsp1.h"

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
static int16_t page_of[MEMMAP_NUM_BLOCKS];     /* its page, else -1 */
/* each page's blocks (a bank's ROM is in several, and mirrored): those of page p are
 * page_blocks[page_first[p] .. page_first[p + 1]) */
#define PAGES_MAX (0x600000 / 0x8000)   /* (MAX_ROM_SIZE in 32 KB pages) */
static uint16_t page_first[PAGES_MAX + 1], page_blocks[MEMMAP_NUM_BLOCKS];
static int8_t slot_of[PAGES_MAX];       /* the slot a page is in, else -1 */
static struct slot { uint8 *data; int page; uint32_t used; } slots[96];
static int nslots;
static uint32_t page_tick, page_reads, page_runs;

static void slot_map(const struct slot *s, bool in)
{
    for (int i = page_first[s->page]; i < page_first[s->page + 1]; i++) {
        int b = page_blocks[i];

        Memory.Map[b] = in ? s->data + (vrom[b] - s->page * (int)page_size)
                                 - ((b << MEMMAP_SHIFT) & 0xffff)
                           : (uint8 *)MAP_PAGED;
    }
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
    int page = page_of[b];
    struct slot *s;

    if (page < 0)                       /* (not a paged block: as it is) */
        return Memory.Map[b];
    if (slot_of[page] >= 0)
        s = &slots[slot_of[page]];
    else {                              /* the least used that nothing reads from */
        s = NULL;
        for (int i = 0; i < nslots; i++)
            if (!slot_busy(&slots[i]) && (!s || slots[i].used < s->used))
                s = &slots[i];
        if (!s) {
            kit_log("snes9x: every ROM page is in use");
            exit(1);
        }
        if (s->page >= 0) {
            slot_map(s, false);
            slot_of[s->page] = -1;
        }
        fseek(rom_file, rom_skip + (long)page * (long)page_size, SEEK_SET);
        fread(s->data, 1, page_size, rom_file);
        s->page = page;
        slot_of[page] = (int8_t)(s - slots);
        page_reads++;
    }
    s->used = ++page_tick;
    slot_map(s, true);
    return Memory.Map[b];
}

static void pages_setup(void)
{
    int pages;

    page_size = Memory.HiROM ? 0x10000 : 0x8000;
    pages = (int)((s9x_rom_paged + page_size - 1) / page_size);
    for (int b = 0; b < MEMMAP_NUM_BLOCKS; b++) {
        uint8 *m = Memory.Map[b];
        int32_t off;

        vrom[b] = -1;
        page_of[b] = -1;
        if (!Memory.BlockIsROM[b] || m < (uint8 *)MAP_LAST)
            continue;
        off = (int32_t)(m - Memory.ROM) + ((b << MEMMAP_SHIFT) & 0xffff);
        if (off < HEAD || off >= (int32_t)s9x_rom_paged)
            continue;
        vrom[b] = off;
        page_of[b] = (int16_t)(off / (int)page_size);
        page_first[page_of[b] + 1]++;
        Memory.Map[b] = (uint8 *)MAP_PAGED;
    }
    for (int p = 0; p < pages; p++) {   /* counts to starts */
        page_first[p + 1] += page_first[p];
        slot_of[p] = -1;
    }
    {
        uint16_t at[PAGES_MAX];

        memcpy(at, page_first, sizeof(at));
        for (int b = 0; b < MEMMAP_NUM_BLOCKS; b++)
            if (page_of[b] >= 0)
                page_blocks[at[page_of[b]]++] = (uint16_t)b;
    }
    /* as many as fit, wherever there is a block for one (the memory freed after the load
     * is in pieces), while KEEP_FREE stays free in all */
    while (nslots < (int)(sizeof(slots) / sizeof(slots[0])) &&
           (size_t)mallinfo().fordblks > page_size + KEEP_FREE &&
           kit_heap_largest() >= page_size &&
           (slots[nslots].data = malloc(page_size)) != NULL)
        slots[nslots++].page = -1;
}

static void pages_age(void)
{
    for (int i = 0; i < nslots; i++)
        if (slots[i].page >= 0)
            slot_map(&slots[i], false);
}

/* A plain cartridge's battery RAM, 128 KB from the core's Init whatever the game,
 * cut to its own size (Super Metroid's 8 KB): the rest is room for ROM pages. Not
 * for a chip's games, nor where Map points into it rather than through its mask */
extern uint8 *s9x_dma_buffer;           /* (dma.c, prepare.py 9) */

static void sram_trim(void)
{
    const uint8 *lo = Memory.SRAM, *hi = Memory.SRAM + 0x20000;
    unsigned size = Memory.SRAMSize ? (1u << (Memory.SRAMSize + 3)) * 128 : 0;
    uint8 *n;

    if (Settings.SA1 || Settings.SuperFX || Settings.SDD1 || Settings.C4 || Settings.SRTC ||
        Settings.BS || size >= 0x20000)
        return;
    for (int b = 0; b < MEMMAP_NUM_BLOCKS; b++)
        if ((Memory.Map[b] >= lo && Memory.Map[b] < hi) ||
            (Memory.WriteMap[b] >= lo && Memory.WriteMap[b] < hi))
            return;
    if (size < 0x800)
        size = 0x800;
    if ((n = realloc(Memory.SRAM, size)) != NULL)
        CPU.Memory_SRAM = Memory.SRAM = n;
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
static uint16_t tapped;                 /* buttons pressed since the last poll */

/* A key down and up again between two polls (a slow frame takes 150 ms) still
 * presses its button for this frame: else the two cancel and the tap is lost */
static void input_poll(void)
{
    int code;
    bool down;

    tapped = 0;
    while (kit_key(&code, &down))
        if ((unsigned)code < 128 && pad_of[code]) {
            uint8_t *n = &held[pad_of[code] - 1];

            if (down) {
                (*n)++;
                tapped |= 1u << (pad_of[code] - 1);
            } else if (*n)
                (*n)--;
        }
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    return port == 0 && device == RETRO_DEVICE_JOYPAD && id < 16 &&
           (held[id] || (tapped >> id & 1));
}

/* ---- the frame ------------------------------------------------------------------------- */
static uint32_t frames;

/* Whether the core draws the next frame (prepare.py 7): not when the sound queued for
 * the speaker is below LOW (the emulation is behind: the time goes to the CPU and the
 * sound); at least every MAX_SKIP + 1st frame is drawn all the same */
#define MAX_SKIP 6
int s9x_render = 1;
static uint32_t skipped, skipped_all;

static void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (data)
        kit_present565(data, (int)width, (int)height, (int)(pitch / 2));
    frames++;
}

/* ---- the sound: a ring of stereo frames ------------------------------------------------ */
#define RATE 32000                      /* (the core's is 32040: near enough) */
#define RING 4096                       /* frames: 0.13 s */
#define FRAME (RATE / (Settings.PAL ? 50 : 60))        /* what a frame makes, about */
#define TARGET (2048 + 2 * FRAME)       /* the kit's 64 ms and two frames of ours: a page
                                         * read from the card can hold a frame up */
#define LOW (2048 - FRAME / 4)          /* on time: the kit about full */

static int16_t ring[RING * 2];
static volatile unsigned ring_in, ring_out;
static volatile uint32_t dry;          /* times the sound thread found nothing (the log) */
static volatile bool sound_ending;
static int16_t last_l, last_r;

/* The kit's sound thread, for its next chunk. It keeps up to 64 ms ahead of the speaker
 * (kit_audio_queued), so a chunk the game has not made yet is waited for while the
 * kit still has SAFE frames for the speaker (a frame comes in one go of ~534):
 * silence put in its place was a crack each time, and made the queue look fuller
 * than its sound. Only then the rest fades out from the last sample, not a step to 0 */
#define SAFE 512                        /* frames: 16 ms */

static void render(int16_t *lr, unsigned n)
{
    unsigned out = ring_out;

    while (ring_in - out < n && !sound_ending && kit_audio_queued() > SAFE)
        kit_sleep(2);
    for (unsigned i = 0; i < n; i++) {
        if (out == ring_in) {
            dry++;
            for (; i < n; i++) {
                last_l = (int16_t)(last_l * 7 / 8);
                last_r = (int16_t)(last_r * 7 / 8);
                lr[2 * i] = last_l;
                lr[2 * i + 1] = last_r;
            }
            break;
        }
        lr[2 * i] = last_l = ring[2 * (out & (RING - 1))];
        lr[2 * i + 1] = last_r = ring[2 * (out & (RING - 1)) + 1];
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

/* ---- what the core asks of the frontend ------------------------------------------------ */
static bool environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
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
    uint32_t runs = 0;

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
#ifdef ASMCPU
    /* what the assembly CPU reads in CPU (snes9x.h) and nothing in the core sets */
    CPU.DSPGet = (void *)S9xGetDSP;
    CPU.DSPSet = (void *)S9xSetDSP;
#endif
    if ((Settings.SA1 || Settings.SDD1) && !(s9x_dma_buffer = malloc(0x10000))) {
        kit_log("snes9x: no room for the chip's 64 KB buffer");
        return 1;
    }
    sram_trim();
    if (s9x_rom_paged) {
        char text[112];

        if (Settings.SA1 || Settings.SuperFX || Settings.SDD1 || Settings.C4) {
            kit_log("snes9x: a game with its own chip has to fit whole");
            return 1;
        }
        pages_setup();
        snprintf(text, sizeof(text), "snes9x: %ld KB ROM in %u KB pages, %d of them in memory "
                 "(%u KB free after them, the largest piece %u KB)", size / 1024, page_size / 1024, nslots,
                 (unsigned)(mallinfo().fordblks / 1024), (unsigned)(kit_heap_largest() / 1024));
        kit_log(text);
    }
    snprintf(srm_path, sizeof(srm_path), "%s", argv[1]);
    if ((dot = strrchr(srm_path, '.')) != NULL)
        *dot = 0;
    strncat(srm_path, ".srm", sizeof(srm_path) - strlen(srm_path) - 1);
    srm(false);

    kit_audio_start(RATE, render);
    /* Paced by the sound: a frame whenever what is queued for the speaker (the kit's
     * and ours) is below TARGET, so the game keeps the speaker's clock and never runs
     * dry or ahead. Below LOW it is behind: the frame is not drawn */
    while (!kit_quit_asked()) {
        unsigned queued = kit_audio_queued() + (ring_in - ring_out);

        if (queued > TARGET) {
            kit_sleep(1 + (queued - TARGET) / (RATE / 1000));
            continue;
        }
        s9x_render = queued >= LOW || skipped >= MAX_SKIP;
        skipped = s9x_render ? 0 : skipped + 1;
        skipped_all += !s9x_render;
        retro_run();
        if (++runs % 600 == 0) {         /* 10 s of the SNES's time: for the log */
            char text[96];

            snprintf(text, sizeof(text), "snes9x: %u of 600 frames not drawn, the sound ran dry %u times",
                     (unsigned)skipped_all, (unsigned)dry);
            kit_log(text);
            skipped_all = dry = 0;
        }
        if (s9x_rom_paged && ++page_runs % AGE == 0)
            pages_age();
    }
    sound_ending = true;
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

