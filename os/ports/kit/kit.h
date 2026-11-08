/* The game kit: what a game or an emulator built as a B310E OS app sees of
 * the OS. Its code is ordinary C on newlib (stdio, malloc, math): files
 * are the card's (relative paths in the app's folder), stdout and stderr
 * go to the OS log, malloc gets the block the kit took for the game.
 * The game's main() is kit_main(), run in a thread of the app's below the
 * UI; returning or exit() ends the game and the app.
 *
 * Nothing here includes the OS's headers: the OS side (kit_os.c, and the
 * game's own OS-side file) has the SDK's app.h, which defines malloc,
 * memcpy ... as the API's.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIT_H
#define KIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the screen -------------------------------------------------------------------
 * The LCD is 128 x 160 (portrait). A view turns it: KIT_ROT_90 puts the
 * game's top at the LCD's right edge (the phone held sideways, the keypad
 * to the right), so the game sees 160 x 128. Frames are scaled to it:
 * KIT_FIT whole, with bars; KIT_FILL stretched to the whole screen;
 * KIT_CROP to the whole screen, the edges cut; KIT_ONE 1:1, centred.
 * smooth: no line of the frame is lost above half size: most stay as they
 * are, the rest are the mean of two, or (kit_game.row_grid) go where a tile
 * has nothing to lose (else nearest). */
enum { KIT_ROT_0, KIT_ROT_90, KIT_ROT_180, KIT_ROT_270 };
enum { KIT_FIT, KIT_FILL, KIT_CROP, KIT_ONE };
enum { KIT_KEYS_TURNED, KIT_KEYS_AS_IS };       /* the D-pad: with the view, or not */

typedef struct {
    uint8_t rotate, scale, smooth, keys;
} kit_view_t;

#define KIT_LCD_W 128
#define KIT_LCD_H 160

void kit_set_view(const kit_view_t *v);
void kit_get_view(kit_view_t *v);
/* the colours of an 8-bit frame: n RGB triplets, 0..255 */
void kit_palette(const uint8_t *rgb, int n);
void kit_present8(const uint8_t *src, int w, int h, int pitch);
void kit_present565(const uint16_t *src, int w, int h, int pitch);  /* pitch in pixels */
/* The same a line at a time, for a console that draws them in turn: no frame
 * of its own to fill and read back. Lines top to bottom (0 .. h - 1), 32-bit
 * aligned; the last one's must stay, the one before may be drawn over. */
void kit_begin565(int w, int h);
void kit_line565(int y, const uint16_t *line);
void kit_end565(void);

/* ---- keys (the codes of os/drivers.h) -------------------------------------------- */
enum {
    KIT_DIAL = 0x01, KIT_END = 0x02, KIT_UP = 0x04, KIT_DOWN = 0x05,
    KIT_LEFT = 0x06, KIT_RIGHT = 0x07, KIT_LSOFT = 0x08, KIT_RSOFT = 0x09,
    KIT_CENTER = 0x0d, KIT_HASH = 0x23, KIT_STAR = 0x2a, KIT_0 = 0x30,
};
/* The next press or release; the D-pad turned with the view (keys). */
bool kit_key(int *code, bool *down);

/* ---- sound: 16-bit stereo at rate, pulled from render in a thread of
 * the kit's above the game (the music the OS plays is paused) ---------------------- */
int kit_audio_start(unsigned rate, void (*render)(int16_t *lr, unsigned frames));
void kit_audio_stop(void);

/* ---- the rest -------------------------------------------------------------------- */
uint32_t kit_ms(void);                  /* since boot */
void kit_sleep(uint32_t ms);
bool kit_quit_asked(void);              /* the screen was left: save and end */
const char *kit_dir(void);              /* the game's folder: relative paths */
void kit_log(const char *line);
size_t kit_heap_largest(void);          /* the largest block malloc can get */
unsigned kit_irq_save(void);            /* for state the sound thread shares */
void kit_irq_restore(unsigned s);

/* the game's */
int kit_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
