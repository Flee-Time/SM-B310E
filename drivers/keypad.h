/*
 * B310E-OS — drivers/keypad.h
 *
 * Matrix keypad driver for the Spreadtrum SC6530C on-chip keypad
 * controller at 0x87000000 (NOT the ADC). Polled scan, no interrupts in
 * v1; presses are edge-detected, translated through the static B310E
 * keymap and pushed into keypad_queue for consumer tasks (the integration
 * wave creates a task that blocks on the queue).
 *
 * ---- hardware (verified against fpdoom syscode.c/syscode.h, Unlicense) ----
 *   - Register block @ 0x87000000; offsets below match fpdoom syscode.h:27-32
 *     exactly (see keypad_base_t).
 *   - APB power bit 0x80040 @ 0x8b0000a0. Chip init already set it; the
 *     driver re-asserts it defensively (OR, never a plain write).
 *  - SC6530/SC6531 expose only matrix rows/cols 2-7: fpdoom masks the
 *     ctrl row/col bitmasks with 0xfc (syscode.c:882-883, "why?").
 *   - Scan decode (fpdoom sys_event, syscode.c:908-955): pressed column =
 *     int_raw bit; that column's row bits live in key_status byte (col&3);
 *     the key index is the 7-bit composite k = (rowbyte & 0x70) >> 1 |
 *     (rowbyte & 7), then a lookup into the 64-byte translation table
 *     s_keytrn (built like fpdoom sys_getkeymap from the real firmware
 *     keymap). Status byte bit 3 is a controller status bit, not a row —
 *     fpdoom skips the frame on it (`if (status & 8) return EVENT_END`).
 *
 *  ---- EIC power button = B310E hangup/END (fpdoom syscode.c:841-868) -----
 * The stock keymap has NO KEY_END entry — END is not in the matrix. On
 * Samsung feature phones the hangup button IS the power button, delivered
 * via the External Interrupt Controller (EIC), NOT the keypad controller:
 *   - EIC_DBNC_DATA @ 0x82001900, channel 3 (SC6530C, active-high, NOT
 *     inverted — the ^=1 in fpdoom is SC6531DA-only).
 *   - keypad_poll() reads it and emits KEY_END on the 0 -> 1 press edge
 *     (edge-triggered, no repeat while held, re-arms on release).
 *   - keypad_init() unmasks the channel (fpdoom eic_enable, syscode.c:
 *     851-868, SC6530 path: 0x820010e4=0x20, 0x820010e0=0x80, then
 *     EIC_DBNC_DMSK @ 0x82001904 |= 1<<3).
 *
 * ---- REAL B310E keymap (extracted from dump_firmware.bin @ 0xc6e70) ------
 * Raw dump table keymap[col*8+row] -> key code (0xffff = empty), transposed
 * into s_keytrn[row*8+col] exactly like fpdoom sys_getkeymap (rotate=0, the
 * identity num_turn). Codes cross-checked against fptest on this phone.
 *
 *              col0    col1    col2    col3    col4
 *  row0        CENTER  DIAL    -       -       LEFT
 *  row1        1       2       3       LSOFT   DOWN
 *  row2        4       5       6       RSOFT   -
 *  row3        7       8       9       -       UP
 *  row4        *       0       #       -       RIGHT
 *
 * Priority when several keys are pressed at once = scan order (lowest
 * column first) — the first decoded key wins.
 *
 * Key codes are the fpdoom KEYPAD_ENUM values (syscode.h:130-139) so the
 * integration wave and future apps rely on stable codes.
 *
 * ---- event encoding (KEY_EVENT_UP_FLAG) ----------------------------------
 * keypad_queue carries uint32_t events: the low byte (0x00-0xff) is the
 * KEY_* code, and bit 8 (KEY_EVENT_UP_FLAG) marks a RELEASE ("key up")
 * event. A plain code is a PRESS ("key down") event. This mirrors the
 * fpdoom sys_event model (syscode.c:908-955) exactly: the SC6530 keypad
 * controller is EDGE-TRIGGERED and encodes the edge in the int_raw
 * column bits — bits 0-3 = KEY DOWN events for columns 0-3, bits 4-7 =
 * KEY UP events for columns 4-7 (column i's release at bit i+4, decoding
 * the same key_status row byte via the (i & 3) wrap). The controller
 * reports nothing while a key is held, so no software held-state is
 * needed; the EIC power button is handled separately (s_pb_prev edge
 * tracking).
 */

#ifndef B310E_OS_KEYPAD_H
#define B310E_OS_KEYPAD_H

#include <stdint.h>
#include "kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- key codes (KEYPAD_ENUM + B310E extensions) ------------------------ */

enum {
    KEY_DIAL    = 0x01,
    KEY_UP      = 0x04,
    KEY_DOWN    = 0x05,
    KEY_LEFT    = 0x06,
    KEY_RIGHT   = 0x07,
    KEY_LSOFT   = 0x08,
    KEY_RSOFT   = 0x09,
    KEY_CENTER  = 0x0d,
    KEY_HASH    = 0x23,
    KEY_VOLUP   = 0x24,
    KEY_VOLDOWN = 0x25,
    KEY_STAR    = 0x2a,
    KEY_0       = 0x30,
    KEY_1       = 0x31,
    KEY_2       = 0x32,
    KEY_3       = 0x33,
    KEY_4       = 0x34,
    KEY_5       = 0x35,
    KEY_6       = 0x36,
    KEY_7       = 0x37,
    KEY_8       = 0x38,
    KEY_9       = 0x39,

    /* B310E extensions — not present in the stock KEYPAD_ENUM */
    KEY_END        = 0x02,  /* call end (often the EIC power line)  */
    KEY_MUSIC_PLAY = 0x40,  /* music player: play/pause             */
    KEY_MUSIC_NEXT = 0x41,  /* music player: next track             */
    KEY_MUSIC_PREV = 0x42   /* music player: previous track         */
};

/* Key events pushed to keypad_queue are uint32_t: values 0x00-0xff are
 * KEY_* codes (press / "down" events); bit 8 marks a release / "up"
 * event. Consumers test with keypad_event_is_up() and extract the code
 * with keypad_event_code(). */
#define KEY_EVENT_UP_FLAG 0x100u

/* ---- hardware register block (fpdoom syscode.h:27-32, Unlicense) ------- */
/* Offsets: ctrl 0x00, int_en 0x04, int_raw 0x08, int_mask 0x0c,
 * int_clr 0x10, (dummy) 0x14, polarity 0x18, debounce 0x1c, long_key 0x20,
 * sleep_cnt 0x24, clk_divide 0x28, key_status 0x2c. Volatile-qualified as
 * in the source; every access is a hardware read/write. */

#define KEYPAD_BASE_ADDR   0x87000000u
#define KEYPAD_APB_PWR_REG 0x8b0000a0u
#define KEYPAD_APB_PWR_BITS 0x80040u   /* keypad APB power gate bit      */

#define KEYPAD_COL_BITS    0xffu       /* int_raw: 8 column bits         */
#define KEYPAD_MASK_FC     0xfcu       /* SC6530/31 usable mask (0xfc)   */
#define KEYPAD_INT_ALL     0xfffu       /* int_en / int_clr full set     */
#define KEYPAD_DEBOUNCE    16u
#define KEYPAD_CLK_DIVIDE  1u
#define KEYPAD_POLARITY    0xffffu

#define KEYPAD_MATRIX_COLS 8u          /* int_raw bits 0-7 (decode loop)   */
#define KEYPAD_MATRIX_ROWS 8u          /* key_status bytes 0-3, 8 rows each */
#define KEYPAD_QUEUE_CAP   16u         /* key event ring capacity        */

typedef volatile struct {
    uint32_t ctrl, int_en, int_raw, int_mask;
    uint32_t int_clr, dummy_14, polarity, debounce;
    uint32_t long_key, sleep_cnt, clk_divide, key_status;
    uint32_t sleep_stat, dbg_stat1, dbg_stat2;
} keypad_base_t;

/* ---- API --------------------------------------------------------------- */

/* Key event queue: keypad_poll() pushes a KEY_* code (press, "down") on
 * every new press edge and KEY_* | KEY_EVENT_UP_FLAG (release, "up") on
 * every release edge; consumers pop with q_pop() and block/wake on it. */
extern msg_queue_t keypad_queue;

/* Module descriptor for the kernel module framework. Nothing references
 * it yet (gc-sections drops it); the integration wave wires it:
 *   module_register(&keypad_module);  then  module_init_all();  or call
 * keypad_init() directly. */
extern const module_t keypad_module;

/* APB power on, matrix row/col config from the real keymap (s_keytrn),
 * debounce, enable. Initializes keypad_queue. Returns 0 on success, -1 on
 * failure. */
int keypad_init(void);

/* Non-blocking scan: returns a KEY_* code on a new press edge (int_raw
 * bits 0-3), KEY_* | KEY_EVENT_UP_FLAG on a release edge (int_raw bits
 * 4-7), and returns 0 when nothing happened. The event is pushed into
 * keypad_queue. The controller is edge-triggered — while a key is held
 * it reports nothing (no repeat in v1; long-press is a later wave). */
int keypad_poll(void);

/* Pause/resume the keypad matrix SCAN (the controller enable bit). Pause
 * around ANA/ADI critical reads (battery ADC, SD probe): a HELD key keeps
 * a driven current path through the matrix, and the scan + an ANA
 * operation contends and HANGS the phone (HW-verified 2026-08-21).
 * keypad_resume() clears stale edges raised while disabled. */
void keypad_pause(void);
void keypad_resume(void);

/* Re-assert the EIC power/END channel 3 debounce config (fpdoom
 * eic_enable: 0x820010e4=0x20, 0x820010e0=0x80, DMSK ch3 unmask). The
 * battery driver's ADC recovery rewrites 0x820010E0/E4 (EIC channels
 * 4/5) and would otherwise leave the END key dead after a battery read. */
void keypad_eic_reapply(void);

/* Raw EIC power/END button level (1 = held). The edge events alone cannot
 * tell when the key is still down; the boot menu uses this for its
 * hold-to-reboot gesture. */
int keypad_pb_held(void);

/* Printable key name: "UP","DOWN","LEFT","RIGHT","CENTER","LSOFT",
 * "RSOFT","DIAL","END","0".."9","*","#","VOLUP","VOLDOWN","MUSIC_*"...
 * Unknown codes yield "?". */
const char *key_name(int code);

/* True when ev is a release (key-up) event, i.e. KEY_EVENT_UP_FLAG set. */
int keypad_event_is_up(uint32_t ev);

/* The KEY_* code of an event (low byte, strips the up flag). */
int keypad_event_code(uint32_t ev);

/* Printable event name: key_name(code) plus "_Down" or "_Up", e.g.
 * "LSOFT_Down", "LSOFT_Up", "END_Down", "END_Up", "5_Up". Small static
 * buffer — print/copy immediately, never nest calls (same contract as
 * key_name). */
const char *key_name_event(uint32_t ev);

/* Raw-bits -> key code decode. rowbits = key_status, colbits = int_raw.
 * Pure logic (no hardware access): host tests call this directly. */
int keypad_decode_for_test(uint32_t rowbits, uint32_t colbits);

/* Pure EIC power-button edge logic: returns KEY_END on a 0 -> 1 press
 * edge, KEY_END | KEY_EVENT_UP_FLAG on a 1 -> 0 release edge, else 0.
 * Host tests call this directly; keypad_poll() feeds it the live EIC
 * level. */
int keypad_pb_event_for_test(uint32_t pb_val, uint32_t prev);

/* Back-compat press-edge-only helper (pre-up-event API): returns KEY_END
 * when the button goes from released (prev == 0) to held (pb_val != 0),
 * else 0. Kept so older host tests keep compiling; keypad_poll() uses
 * keypad_pb_event_for_test(). */
int keypad_pb_to_end_for_test(uint32_t pb_val, uint32_t prev);

#ifdef HOST_TEST
/* Register injection for host tests: keypad_poll() decodes these instead
 * of reading the controller when HOST_TEST is defined. */
extern uint32_t keypad_test_raw;
extern uint32_t keypad_test_status;
extern uint32_t keypad_test_pb;   /* injected EIC power-button level */
#endif

#ifdef __cplusplus
}
#endif

#endif /* B310E_OS_KEYPAD_H */
