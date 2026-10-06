/*
 * B310E-OS — drivers/keypad.c
 *
 * Keypad module: SC6530C matrix scan @ 0x87000000 + key event queue, plus
 * the EIC power button (0x82001900, channel 3) which IS the B310E hangup/
 * END key — the stock matrix has no END entry.
 *
 * Register init/decode copied from fpdoom syscode.c:870-955 (Unlicense /
 * public domain — verbatim reuse is explicitly permitted), adapted from
 * the firmware-derived keymap to the static B310E map in this file.
 * The EIC power-button read is fpdoom keypad_read_pb (syscode.c:841-848)
 * and eic_enable (syscode.c:851-868).
 *
 * SAFETY: this driver only touches the on-chip keypad controller
 * (0x87000000), the APB power gate (0x8b0000a0) and the ADI/EIC analog
 * mailbox (0x8200xxxx). It NEVER writes pin-mux registers (0x8cxxxxxx) —
 * in particular NEVER 0x8c0002a4 (UART-TX pinmux hangs the phone, fpdoom
 * syscode.c:189-197).
 */

#include "keypad.h"
#include "kernel.h"
#include "../arch/chip.h"   /* MEM4() */

/* ---- ctrl bitfields (fpdoom syscode.c:895-902) ------------------------- */

#define KEYPAD_CTRL_ENABLE  (1u << 0)   /* controller enable            */
#define KEYPAD_CTRL_SLEEP   (1u << 1)   /* sleep mode (we disable it)    */
#define KEYPAD_CTRL_LONG    (1u << 2)   /* long-press detection enable  */
#define KEYPAD_CTRL_ROW_SHIFT 16u
#define KEYPAD_CTRL_COL_SHIFT 8u

/* ---- real B310E keymap (fpdoom sys_getkeymap port, syscomm.c:312-349) --- */
/* Extracted from the stock firmware dump (dump_firmware.bin, keymap @
 * 0xc6e70; header 0x4d/0x400/0xa @ 0xc6e64). The short[64] table is the
 * RAW matrix: keymap[col * 8 + row] -> key code, 0xffff = empty.
 *
 * fpdoom sys_getkeymap transposes it into the 64-byte translation table
 * with rotate = 0 (num_turn[0] = {UP,DOWN,LEFT,RIGHT,1..9} is the
 * identity for rotate 0, so NO remapping happens on this phone):
 *
 *     dest[j * 8 + i] = keymap[i * nrow + j]      (i = col, j = row)
 *
 * dest is indexed by k = row * 8 + col (the exact index fpdoom sys_event
 * computes from the key_status byte, syscode.c:932-945). 0xff = no key.
 *
 * RAW B310E matrix (dump @ 0xc6e70), verified on hardware via fptest:
 *              col0    col1    col2    col3    col4
 *  row0        CENTER  DIAL    -       -       LEFT
 *  row1        1       2       3       LSOFT   DOWN
 *  row2        4       5       6       RSOFT   -
 *  row3        7       8       9       -       UP
 *  row4        *       0       #       -       RIGHT
 *
 * fptest ground-truth (physical -> code): CENTER 0x0d, digits 0x30-0x39,
 * STAR 0x2a, HASH 0x23, LSOFT 0x08, RSOFT 0x09, UP 0x04, DOWN 0x05,
 * LEFT 0x06, RIGHT 0x07, DIAL 0x01 — all match the table below. */
static const uint8_t s_keytrn[64] = {
    /* k = row*8+col; row0: CENTER, DIAL, -, -, LEFT, -, -, - */
    0x0d, 0x01, 0xff, 0xff, 0x06, 0xff, 0xff, 0xff,
    /* row1: 1, 2, 3, LSOFT, DOWN, -, -, - */
    0x31, 0x32, 0x33, 0x08, 0x05, 0xff, 0xff, 0xff,
    /* row2: 4, 5, 6, RSOFT, -, -, -, - */
    0x34, 0x35, 0x36, 0x09, 0xff, 0xff, 0xff, 0xff,
    /* row3: 7, 8, 9, -, UP, -, -, - */
    0x37, 0x38, 0x39, 0xff, 0x04, 0xff, 0xff, 0xff,
    /* row4: *, 0, #, -, RIGHT, -, -, - */
    0x2a, 0x30, 0x23, 0xff, 0x07, 0xff, 0xff, 0xff,
    /* rows 5-7: unused */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

/* ---- module state ------------------------------------------------------ */

msg_queue_t keypad_queue;                   /* extern (keypad.h)          */
static uint32_t s_key_buf[KEYPAD_QUEUE_CAP];
static uint32_t s_pb_prev;                  /* last EIC power-button level */

#ifdef HOST_TEST
uint32_t keypad_test_raw;                   /* injected int_raw           */
uint32_t keypad_test_status;                /* injected key_status        */
uint32_t keypad_test_pb;                    /* injected EIC power button  */
#endif

/* ---- ADI mailbox (EIC power-button reads/writes) ------------------------ */
/* fpdoom adi_read / adi_write, chip-2 path (SC6530, syscode.c:33-67):
 *   read:  MEM4(0x82000018) = addr & 0xfff; while (MEM4(0x8200001c) >> 31);
 *          return MEM4(0x8200001c) & 0xffff;
 *   write: wait FIFO-FULL clear (0x82000020 & 1<<9), MEM4(addr) = val,
 *          wait FIFO-EMPTY clear (0x82000020 & 1<<8).
 * Identical to the mailbox the LCD backlight uses (drivers/lcd.c
 * adi_read32/adi_write32); this is a small local copy to keep the two
 * drivers independent. */

#define KEYPAD_ADI_RD_CMD    0x82000018u
#define KEYPAD_ADI_RD_DATA   0x8200001cu
#define KEYPAD_ADI_FIFO_STS  0x82000020u   /* SC6530 FIFO status (chip 2) */
#define KEYPAD_ADI_FIFO_FULL (1u << 9)
#define KEYPAD_ADI_FIFO_EMPTY (1u << 8)
#define KEYPAD_ADI_BUDGET    1000000u       /* bounded: never freeze sched */

#ifndef HOST_TEST
static uint32_t keypad_adi_read(uint32_t addr)
{
    uint32_t a = 0, n = KEYPAD_ADI_BUDGET;

    MEM4(KEYPAD_ADI_RD_CMD) = addr & 0xfffu;
    while ((a = MEM4(KEYPAD_ADI_RD_DATA)) >> 31)  /* wait busy clear */
        if (--n == 0) break;
    return a & 0xffffu;
}

static void keypad_adi_write(uint32_t addr, uint32_t val)
{
    uint32_t n = KEYPAD_ADI_BUDGET;

    while (MEM4(KEYPAD_ADI_FIFO_STS) & KEYPAD_ADI_FIFO_FULL)  /* FIFO full  */
        if (--n == 0) return;
    MEM4(addr) = val;
    n = KEYPAD_ADI_BUDGET;
    /* fpdoom adi_write: wait until FIFO_EMPTY is SET (write drained), NOT
     * while it is set (inverted form spins on the normal empty state). */
    while (!(MEM4(KEYPAD_ADI_FIFO_STS) & KEYPAD_ADI_FIFO_EMPTY))
        if (--n == 0) return;
}

static void keypad_adi_write_or(uint32_t addr, uint32_t val)
{
    keypad_adi_write(addr, keypad_adi_read(addr) | val);
}
#endif /* !HOST_TEST */

/* ---- EIC power button (the B310E hangup/END key) ------------------------ */
/* The stock keymap (dump_firmware.bin @ 0xc6e70) has NO 0x02 (KEY_END)
 * entry — END is not part of the keypad matrix. On Samsung feature phones
 * the hangup button IS the power button, delivered via the External
 * Interrupt Controller (EIC). fpdoom keypad_read_pb (syscode.c:841-848):
 * SC6530C reads EIC_DBNC_DATA @ 0x82001900, channel 3, active-high, NOT
 * inverted (the ^=1 inversion exists only on SC6531DA). */

#define KEYPAD_EIC_DBNC_DATA  0x82001900u
#define KEYPAD_EIC_DBNC_DMSK  0x82001904u
#define KEYPAD_EIC_PB_CH      3u

/* 1 while the power button is physically held (EIC debounced data). */
static int keypad_read_pb(void)
{
#ifndef HOST_TEST
    return (int)((keypad_adi_read(KEYPAD_EIC_DBNC_DATA) >>
                  KEYPAD_EIC_PB_CH) & 1u);
#else
    return (int)(keypad_test_pb & 1u);
#endif
}

/* Public raw EIC power/END button level (1 = held). Used by the boot menu
 * for a hold-to-reboot gesture (the edge events alone cannot tell when the
 * key is still down). */
int keypad_pb_held(void)
{
    return keypad_read_pb();
}

/* Pure EIC power-button edge logic (host-testable): returns KEY_END for a
 * 0 -> 1 press edge, KEY_END | KEY_EVENT_UP_FLAG for a 1 -> 0 release
 * edge, else 0. keypad_poll() feeds it the current level plus s_pb_prev. */
int keypad_pb_event_for_test(uint32_t pb_val, uint32_t prev)
{
    if (pb_val && !prev)
        return KEY_END;
    if (!pb_val && prev)
        return KEY_END | (int)KEY_EVENT_UP_FLAG;
    return 0;
}

/* Back-compat press-edge-only helper (pre-up-event API): KEY_END on the
 * 0 -> 1 edge, else 0. Older host tests and callers keep working. */
int keypad_pb_to_end_for_test(uint32_t pb_val, uint32_t prev)
{
    int ev = keypad_pb_event_for_test(pb_val, prev);

    return (ev & (int)KEY_EVENT_UP_FLAG) ? 0 : ev;
}

/* EIC channel 3 (power/END) debounce enable — fpdoom eic_enable,
 * syscode.c:851-868, SC6530 path. The debounced data bit is only valid
 * once the channel is unmasked in EIC_DBNC_DMSK. Exposed as
 * keypad_eic_reapply() so the battery driver can re-assert it after the
 * ADC recovery's config block overwrites 0x820010E0/E4 (EIC channels
 * 4/5). */
#ifndef HOST_TEST
static void keypad_eic_enable(void)
{
    keypad_adi_write(0x820010e4u, 0x20u);
    keypad_adi_write(0x820010e0u, 0x80u);
    keypad_adi_write_or(KEYPAD_EIC_DBNC_DMSK, 1u << KEYPAD_EIC_PB_CH);
}

void keypad_eic_reapply(void)
{
    keypad_eic_enable();
}
#endif /* !HOST_TEST */

/* ---- decode ------------------------------------------------------------ */

/* Decode raw keypad registers into a KEY_* code (0 = none pressed).
 * rowbits = key_status, colbits = int_raw.
 *
 * EXACT fpdoom sys_event() port (syscode.c:920-955):
 *   - the SC6530 keypad controller is EDGE-TRIGGERED and encodes the edge
 *     type in the int_raw column bits: bits 0-3 = KEY DOWN events for
 *     matrix columns 0-3, bits 4-7 = KEY UP events for columns 4-7 (the
 *     release of column i appears at bit i+4).
 *   - a set bit i's row bits live in the key_status byte (i & 3) —
 *     key_status is 4 bytes wide, columns wrap mod 4, so the release bit
 *     i+4 reads the SAME row byte as its press bit i.
 *   - the key index is the 7-bit composite
 *         k = (rowbyte & 0x70) >> 1 | (rowbyte & 7)
 *     (NOT a plain bit number — this is THE difference from the old
 *     decode, which collapsed every key into column 0). k then indexes
 *     the 64-byte translation table s_keytrn (fpdoom keytrn).
 *   - scan order: lowest column first (fpdoom's `for (i = 0; i < 8; i++)`).
 *   - status byte bit 3 is a controller status bit, not a row (fpdoom
 *     `if (status & 8) return EVENT_END`).
 * Returns a PRESS event (KEY_* code) for bits 0-3 and a RELEASE event
 * (KEY_* | KEY_EVENT_UP_FLAG) for bits 4-7, mirroring fpdoom's
 * EVENT_KEYDOWN / EVENT_KEYUP split. */
int keypad_decode_for_test(uint32_t rowbits, uint32_t colbits)
{
    unsigned i;

    if (rowbits & 8u)
        return 0;               /* fpdoom: status bit 3 = no key frame */
    for (i = 0; i < 8; i++) {
        uint32_t byte, k;
        uint8_t code;

        if (!(colbits & (1u << i)))
            continue;
        byte = rowbits >> ((i & 3u) * 8u);   /* this column's row byte */
        k = (byte & 0x70u) >> 1 | (byte & 7u);  /* fpdoom index math */
        code = s_keytrn[k];
        if (code != 0xffu)
            return (i < 4) ? (int)code
                           : (int)code | (int)KEY_EVENT_UP_FLAG;
    }
    return 0;
}

/* Pause/resume the keypad matrix SCAN (the controller enable bit). A held
 * key keeps a sustained current path through the matrix while the scan
 * drives it; combined with an ANA/ADI operation (battery ADC, SD power
 * LDOs + SDCLK_EN) that path contends and HANGS the phone (HW-verified
 * 2026-08-21 — "holding a key + HASH battery read" and "holding DIAL +
 * SD probe" both die silently; releasing the key makes both work). The
 * stock RTOS avoids this with its own pin config / scan discipline; our
 * polled driver pauses the scan around the critical reads so no key,
 * held or not, presents a driven matrix path. Resume clears any stale
 * edges the controller raised while disabled. */
void keypad_pause(void)
{
#ifndef HOST_TEST
    keypad_base_t *kpd = (keypad_base_t *)KEYPAD_BASE_ADDR;

    kpd->ctrl &= ~KEYPAD_CTRL_ENABLE;
#endif
}

void keypad_resume(void)
{
#ifndef HOST_TEST
    keypad_base_t *kpd = (keypad_base_t *)KEYPAD_BASE_ADDR;

    kpd->int_clr = KEYPAD_INT_ALL;
    kpd->ctrl |= KEYPAD_CTRL_ENABLE;
#endif
}

/* ---- init -------------------------------------------------------------- */

int keypad_init(void)
{
#ifndef HOST_TEST
    keypad_base_t *kpd = (keypad_base_t *)KEYPAD_BASE_ADDR;
    unsigned row = 0, col = 0, c, r, ctrl;

    /* Enable exactly the matrix rows/cols used by the real keymap
     * (fpdoom keypad_init, syscode.c:877-880). s_keytrn[k] is non-0xff
     * exactly where keymap[col*8+row] != -1, with k = row*8+col. */
    for (c = 0; c < 8; c++)
        for (r = 0; r < 8; r++)
            if (s_keytrn[r * 8 + c] != 0xff)
                col |= 1u << c, row |= 1u << r;
    /* SC6530/SC6531 only expose matrix rows/cols 2-7 (fpdoom
     * syscode.c:882-883 — rows/cols 0-1 are not scanable on this chip). */
    row &= KEYPAD_MASK_FC;
    col &= KEYPAD_MASK_FC;

    /* APB keypad power — chip init already set 0x80040; re-assert
     * defensively (OR, never clobber sibling power bits). */
    MEM4(KEYPAD_APB_PWR_REG) |= KEYPAD_APB_PWR_BITS;

    kpd->int_clr = KEYPAD_INT_ALL;      /* clear pending key ints  */
    kpd->clk_divide = KEYPAD_CLK_DIVIDE;
    kpd->debounce = KEYPAD_DEBOUNCE;
    kpd->int_en = KEYPAD_INT_ALL;       /* all columns can raise events */
    kpd->polarity = KEYPAD_POLARITY;

    ctrl = kpd->ctrl;
    ctrl |= KEYPAD_CTRL_ENABLE;         /* enable */
    ctrl &= ~KEYPAD_CTRL_SLEEP;         /* no sleep */
    ctrl |= KEYPAD_CTRL_LONG;           /* long-press detect on */
    ctrl &= ~(KEYPAD_MASK_FC << KEYPAD_CTRL_ROW_SHIFT |
              KEYPAD_MASK_FC << KEYPAD_CTRL_COL_SHIFT);  /* clear old */
    ctrl |= row << KEYPAD_CTRL_ROW_SHIFT | col << KEYPAD_CTRL_COL_SHIFT;
    kpd->ctrl = ctrl;

    /* Power/END button lives on the EIC, not the matrix — unmask its
     * debounce channel so keypad_read_pb() sees the real level. */
    keypad_eic_enable();
#endif /* !HOST_TEST */

    q_init(&keypad_queue, s_key_buf, KEYPAD_QUEUE_CAP);
    s_pb_prev = 0;
    return 0;
}

/* ---- poll -------------------------------------------------------------- */

int keypad_poll(void)
{
    uint32_t event = 0, status = 0, pb;
    int first = 0, i;

#ifndef HOST_TEST
    {
        keypad_base_t *kpd = (keypad_base_t *)KEYPAD_BASE_ADDR;

        event = kpd->int_raw & KEYPAD_COL_BITS;
        status = kpd->key_status;
        if (event != 0)
            kpd->int_clr = KEYPAD_INT_ALL;  /* ack + re-arm */
    }
#else
    event = keypad_test_raw & KEYPAD_COL_BITS;   /* test injection */
    status = keypad_test_status;
#endif

    /* The EIC power-button read (an ANA/ADI mailbox transaction) runs on
     * every poll. It is NOT deferred: an earlier "skip while the matrix is
     * active" gate broke END on hardware (key_status carries non-zero idle
     * bits -> the EIC was never read). The held-key + ANA-op + tick freeze
     * is handled at the TASK level instead (the keypad task runs
     * tick-masked via task_yield_tick_masked). */
    pb = (uint32_t)keypad_read_pb();

    /* EIC power button (the B310E hangup/END, NOT in the matrix): a
     * 0 -> 1 edge pushes END_Down (KEY_END) once; a 1 -> 0 edge pushes
     * END_Up (KEY_END | KEY_EVENT_UP_FLAG) once; while held it never
     * repeats. */
    {
        uint32_t pb_ev = (uint32_t)keypad_pb_event_for_test(pb, s_pb_prev);

        if (pb_ev != 0) {
            s_pb_prev = pb;
            q_push(&keypad_queue, pb_ev);
            return (int)pb_ev;
        }
    }
    s_pb_prev = pb;

    /* Matrix: the controller is EDGE-TRIGGERED and reports both edges in
     * int_raw (bits 0-3 = press, bits 4-7 = release — see decode). Push
     * EVERY set-bit event from this frame (multi-key frames deliver all);
     * return the first, mirroring fpdoom's EVENT_KEYDOWN/KEYUP stream. */
    if (status & 8u)
        return 0;               /* fpdoom: status bit 3 = bad frame */

    for (i = 0; i < 8; i++) {
        int ev;

        if (!(event & (1u << i)))
            continue;
        ev = keypad_decode_for_test(status, 1u << i);
        if (ev == 0)
            continue;
        q_push(&keypad_queue, (uint32_t)ev);
        if (first == 0)
            first = ev;
    }
    return first;
}

/* ---- names ------------------------------------------------------------- */

const char *key_name(int code)
{
    static const char s_digits[10][2] = {
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"
    };

    switch (code) {
    case KEY_DIAL:      return "DIAL";
    case KEY_UP:        return "UP";
    case KEY_DOWN:      return "DOWN";
    case KEY_LEFT:      return "LEFT";
    case KEY_RIGHT:     return "RIGHT";
    case KEY_LSOFT:     return "LSOFT";
    case KEY_RSOFT:     return "RSOFT";
    case KEY_CENTER:    return "CENTER";
    case KEY_END:       return "END";
    case KEY_HASH:      return "HASH";
    case KEY_STAR:      return "STAR";
    case KEY_VOLUP:     return "VOLUP";
    case KEY_VOLDOWN:   return "VOLDOWN";
    case KEY_MUSIC_PLAY: return "MUSIC_PLAY";
    case KEY_MUSIC_NEXT: return "MUSIC_NEXT";
    case KEY_MUSIC_PREV: return "MUSIC_PREV";
    default:
        if (code >= KEY_0 && code <= KEY_9)
            return s_digits[code - KEY_0];
        return "?";
    }
}

/* Event name: key_name(code) with a "_DOWN" (press) or "_UP" (release)
 * suffix from the KEY_EVENT_UP_FLAG bit. Small static buffer — same
 * contract as key_name: print/copy immediately, never nest calls. */
const char *key_name_event(uint32_t ev)
{
    static char s_name[16];
    const char *base = key_name(keypad_event_code(ev));
    const char *suffix = keypad_event_is_up(ev) ? "_UP" : "_DOWN";
    unsigned i = 0, s = 0;

    while (base[i] && i < sizeof(s_name) - 5) {
        s_name[i] = base[i];
        i++;
    }
    for (s = 0; suffix[s] && i + s < sizeof(s_name) - 1; s++)
        s_name[i + s] = suffix[s];
    s_name[i + s] = '\0';
    return s_name;
}

/* True when ev is a release (key-up) event. */
int keypad_event_is_up(uint32_t ev)
{
    return (ev & KEY_EVENT_UP_FLAG) != 0;
}

/* The KEY_* code of an event (low byte, strips the up flag). */
int keypad_event_code(uint32_t ev)
{
    return (int)(ev & 0xffu);
}

/* ---- module ------------------------------------------------------------ */

const module_t keypad_module = { "keypad", keypad_init };
