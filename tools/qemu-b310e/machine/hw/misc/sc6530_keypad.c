/*
 * Spreadtrum SC6530C keypad controller + EIC END key (B310E-OS QEMU
 * machine).
 *
 * Todo 17 of .omo/plans/b310e-qemu-machine.md (Wave 4).
 *
 * Two halves, mirroring the firmware driver (drivers/keypad.c/.h):
 *
 * 1. Matrix controller @ 0x87000000 (register offsets = keypad_base_t,
 *    drivers/keypad.h): ctrl +0x0, int_en +0x4, int_raw +0x8, int_mask
 *    +0xc, int_clr +0x10, dummy +0x14, polarity +0x18, debounce +0x1c,
 *    long_key +0x20, sleep_cnt +0x24, clk_divide +0x28, key_status +0x2c,
 *    sleep_stat +0x30, dbg_stat1 +0x34, dbg_stat2 +0x38.
 *
 *    EDGE MODEL (fpdoom sys_event / drivers/keypad.c keypad_poll +
 *    keypad_decode_for_test, the ground truth): the controller is
 *    EDGE-TRIGGERED and encodes the edge in int_raw bits 0-7:
 *      bits 0-3 = KEY DOWN (press) edges, bits 4-7 = KEY UP (release)
 *      edges, where the release of a press that latched slot i (0-3)
 *      latches bit i+4. The pressed key's identity lives in the
 *      key_status 32-bit register: byte (i & 3) of the word is the
 *      "row byte" the decode reads for slot i (the (i & 3) wrap makes
 *      the release read the SAME byte as its press). The row byte
 *      encodes the key position exactly as the firmware decodes it:
 *        k = ((byte & 0x70) >> 1) | (byte & 7)   -- drivers/keypad.c
 *      with our row byte = (row << 4) | (col & 7): bits 6:4 = row,
 *      bits 2:0 = col, so k = row * 8 + col and s_keytrn[k] (the
 *      64-byte translation table embedded in drivers/keypad.c) yields
 *      the KEY_* code. Bit 3 of the word (the "status bit") is never
 *      set by this encoding, so the firmware's `status & 8` bad-frame
 *      check never spuriously trips.
 *
 *      Slot assignment: a press picks DOWN slot (col & 3) when free
 *      (columns 0-3, one slot per column), else the first free slot;
 *      the d-pad column (col 4) therefore uses the first free slot
 *      (0 when idle). The status byte for slot i is byte (i & 3).
 *      Latches stay until the guest writes int_clr (the firmware ACKs
 *      with int_clr = 0xfff after every read, drivers/keypad.c
 *      keypad_poll). The status row byte persists after its edge is
 *      cleared - the guest only decodes it immediately after an edge,
 *      and every press overwrites its own byte, so stale bytes are
 *      harmless (documented simplification).
 *
 *      One key at a time is the normal sendkey flow (down, hold, up);
 *      chorded sends land in distinct slots and decode independently.
 *
 * 2. EIC power button @ 0x82001900 bit 3 = the B310E hangup/END key
 *    (NOT in the matrix - the stock keymap has no KEY_END entry).
 *    The EIC register lives inside the todo-15 ANA bank (sc6530_adi @
 *    0x82001000, 0x2000), and the firmware reads it through the ADI
 *    MAILBOX (keypad_adi_read: RD_CMD 0x82000018 = 0x900, then RD_DATA
 *    0x8200001c), which resolves from the ADI's OWN ana_regs[] array -
 *    an MMIO overlay at 0x82001900 would be invisible to the guest's
 *    read path (the mailbox never consults the address space). The END
 *    key is therefore implemented as a side-effect hook into the ANA
 *    bank: this device holds a QOM link "adi" to the ADI device
 *    (wired by hw/arm/b310e.c) and calls sc6530_adi_set_eic_pb() to
 *    raise/lower bit 3 of the EIC_DBNC_DATA register (0x82001900)
 *    inside the ADI's ANA array - the exact register the guest's
 *    mailbox read observes. The ADI hook gates visibility on the
 *    guest's DMSK unmask write (EIC_DBNC_DMSK 0x82001904 bit 3) and
 *    emits the sc6530_ana_write trace event so END toggles appear in
 *    the audio-observatory trace. (The task text's alternative "END
 *    driven through the same int_raw/status mechanism" was rejected:
 *    END is NOT a matrix key, and the firmware's EIC edge logic reads
 *    the level via the mailbox - a matrix-only model would break
 *    keypad_pb_held() and the END edge events.)
 *
 * HOST INPUT INJECTION (QEMU generic input layer, template hw/input/
 * ps2.c): the device registers a QemuInputHandler (INPUT_EVENT_MASK_KEY)
 * and maps the Linux input-event codes (standard-headers/linux/
 * input-event-codes.h - v11.1.0 delivers Linux KEY_* codes in
 * QemuInputKeyEvent.key, NOT QKeyCodes) to the SC6530 matrix below.
 * The monitor drives it: HMP `sendkey <name>` (QKeyCode names) or
 * QMP `input-send-event`. NOTE: the QKeyCode enum (qapi/ui.json) names
 * the Enter key "ret", not "enter" - `sendkey ret` is the CENTER key
 * on this machine (the plan's literal `sendkey enter` spelling does
 * not parse on v11.1.0, same rename lesson as the boot-mode property).
 *
 * FINAL KEY MAPPING (sendkey name / Linux code / B310E key / code):
 *
 *     ret         KEY_ENTER    28   -> CENTER  0x0d   (row0 col0)
 *     kp_enter    KEY_KPENTER  96   -> DIAL    0x01   (row0 col1)
 *     1..9        KEY_1..KEY_9  2..10 -> 1..9   0x31..0x39
 *     0           KEY_0        11   -> 0       0x30   (row4 col1)
 *     minus       KEY_MINUS    12   -> STAR    0x2a   (row4 col0)
 *     asterisk    KEY_KPASTERISK 55 -> STAR    0x2a   (alias)
 *     slash       KEY_SLASH    53   -> HASH    0x23   (row4 col2)
 *     kp_slash    KEY_KPSLASH  98   -> HASH    0x23   (alias)
 *     f1          KEY_F1       59   -> LSOFT   0x08   (row1 col3)
 *     f2          KEY_F2       60   -> RSOFT   0x09   (row2 col3)
 *     up          KEY_UP      103   -> UP      0x04   (row3 col4)
 *     down        KEY_DOWN    108   -> DOWN    0x05   (row1 col4)
 *     left        KEY_LEFT    105   -> LEFT    0x06   (row0 col4)
 *     right       KEY_RIGHT   106   -> RIGHT   0x07   (row4 col4)
 *     esc         KEY_ESC       1   -> END (EIC bit 3, not the matrix)
 *     end         KEY_END     107   -> END (alias)
 *
 * Every (row, col) entry was verified against s_keytrn (drivers/
 * keypad.c:58): CENTER k=0 -> 0x0d, DIAL k=1 -> 0x01, LSOFT k=11 ->
 * 0x08, RSOFT k=19 -> 0x09, UP k=28 -> 0x04, DOWN k=12 -> 0x05,
 * LEFT k=4 -> 0x06, RIGHT k=36 -> 0x07, STAR k=32 -> 0x2a,
 * HASH k=34 -> 0x23, 0 k=33 -> 0x30.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev.h"
#include "ui/console.h"
#include "ui/input.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "standard-headers/linux/input-event-codes.h"

#define TYPE_SC6530_KEYPAD "sc6530_keypad"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530KeypadState, SC6530_KEYPAD)

/* Matrix register offsets (keypad_base_t, drivers/keypad.h). */
#define SC6530_KPD_CTRL        0x00
#define SC6530_KPD_INT_EN      0x04
#define SC6530_KPD_INT_RAW     0x08
#define SC6530_KPD_INT_MASK    0x0c
#define SC6530_KPD_INT_CLR     0x10
#define SC6530_KPD_DUMMY       0x14
#define SC6530_KPD_POLARITY    0x18
#define SC6530_KPD_DEBOUNCE    0x1c
#define SC6530_KPD_LONG_KEY    0x20
#define SC6530_KPD_SLEEP_CNT   0x24
#define SC6530_KPD_CLK_DIVIDE  0x28
#define SC6530_KPD_KEY_STATUS  0x2c
#define SC6530_KPD_SLEEP_STAT  0x30
#define SC6530_KPD_DBG_STAT1   0x34
#define SC6530_KPD_DBG_STAT2   0x38

#define SC6530_KEYPAD_SIZE     0x40

/* int_raw edge latches: bits 0-3 = DOWN slots, bits 4-7 = their
 * releases (slot i's release at bit i+4 - fpdoom sys_event). */
#define SC6530_KPD_EDGE_ALL    0xffu

/* The guest ACKs edges with int_clr = 0xfff (KEYPAD_INT_ALL). */
#define SC6530_KPD_INT_ALL     0xfffu

/* The ADI device type (declared in hw/misc/sc6530_adi.c). */
#define TYPE_SC6530_ADI        "sc6530_adi"

/* Cross-device EIC hook (defined in hw/misc/sc6530_adi.c): raise/lower
 * the END key level in the ADI's ANA bank at EIC_DBNC_DATA 0x82001900
 * bit 3, the register the guest's mailbox read observes. */
extern void sc6530_adi_set_eic_pb(Object *adi_obj, bool held);

/* ---------------------------------------------------------------------- */
/* Host-key -> SC6530 matrix mapping (see the header comment for the      */
/* full table). row/col = -1 marks the EIC END special case.              */
/* ---------------------------------------------------------------------- */

typedef struct Sc6530KeyMapEntry {
    unsigned int lnx;    /* Linux input-event code (QemuInputKeyEvent.key) */
    const char *name;    /* sendkey name (QKeyCode_str), for logging      */
    int row;             /* SC6530 matrix row, or -1 for the EIC END key  */
    int col;             /* SC6530 matrix column                          */
    uint8_t code;        /* the KEY_* code the guest's s_keytrn yields    */
} Sc6530KeyMapEntry;

static const Sc6530KeyMapEntry sc6530_keymap[] = {
    /* sendkey name   lnx code        row col code */
    { KEY_ENTER,      "ret",          0,  0,  0x0d }, /* CENTER */
    { KEY_KPENTER,    "kp_enter",     0,  1,  0x01 }, /* DIAL   */
    { KEY_1,          "1",            1,  0,  0x31 },
    { KEY_2,          "2",            1,  1,  0x32 },
    { KEY_3,          "3",            1,  2,  0x33 },
    { KEY_4,          "4",            2,  0,  0x34 },
    { KEY_5,          "5",            2,  1,  0x35 },
    { KEY_6,          "6",            2,  2,  0x36 },
    { KEY_7,          "7",            3,  0,  0x37 },
    { KEY_8,          "8",            3,  1,  0x38 },
    { KEY_9,          "9",            3,  2,  0x39 },
    { KEY_0,          "0",            4,  1,  0x30 },
    { KEY_MINUS,      "minus",        4,  0,  0x2a }, /* STAR */
    { KEY_KPASTERISK, "asterisk",     4,  0,  0x2a }, /* STAR (alias) */
    { KEY_SLASH,      "slash",        4,  2,  0x23 }, /* HASH */
    { KEY_KPSLASH,    "kp_slash",     4,  2,  0x23 }, /* HASH (alias) */
    { KEY_F1,         "f1",           1,  3,  0x08 }, /* LSOFT */
    { KEY_F2,         "f2",           2,  3,  0x09 }, /* RSOFT */
    { KEY_UP,         "up",           3,  4,  0x04 },
    { KEY_DOWN,       "down",         1,  4,  0x05 },
    { KEY_LEFT,       "left",         0,  4,  0x06 },
    { KEY_RIGHT,      "right",        4,  4,  0x07 },
    { KEY_ESC,        "esc",         -1, -1,  0x02 }, /* END (EIC bit 3) */
    { KEY_END,        "end",         -1, -1,  0x02 }, /* END (alias) */
};

static const Sc6530KeyMapEntry *sc6530_keymap_lookup(unsigned int lnx)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(sc6530_keymap); i++) {
        if (sc6530_keymap[i].lnx == lnx) {
            return &sc6530_keymap[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Device state                                                           */
/* ---------------------------------------------------------------------- */

struct Sc6530KeypadState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion matrix_iomem;     /* 0x87000000 */
    QemuInputHandlerState *hs;     /* generic-input registration */

    Object *adi;                   /* link: the ADI device (EIC END bit) */

    /* Matrix registers (store+echo). */
    uint32_t ctrl;
    uint32_t int_en;
    uint32_t int_mask;
    uint32_t polarity;
    uint32_t debounce;
    uint32_t long_key;
    uint32_t sleep_cnt;
    uint32_t clk_divide;
    uint32_t dummy;
    uint32_t sleep_stat;
    uint32_t dbg_stat1;
    uint32_t dbg_stat2;

    /* Edge latches + status row bytes (see the header comment). */
    uint32_t int_raw;
    uint32_t key_status;

    /* Key currently held in each DOWN slot (0-3); -1 = slot free. */
    int held[4];

    /* EIC END key current level (1 = held). */
    bool end_held;
    /* -M b310e,hold-end=on: assert the END level from reset (a physically
     * held key). The guest sees it once it unmasks EIC_DBNC_DMSK bit 3. */
    bool hold_end;
    QEMUTimer *hold_end_timer;
};

/* ---------------------------------------------------------------------- */
/* Matrix model                                                           */
/* ---------------------------------------------------------------------- */

/* Free DOWN slot for a press of (row, col): prefer the key's own column
 * (columns 0-3 map 1:1 to slots), else the first free slot (the d-pad
 * column 4, which has no dedicated slot - its identity lives in the
 * row byte). Returns -1 when every slot is busy. */
static int sc6530_keypad_find_slot(Sc6530KeypadState *s, int col)
{
    int i, preferred = (col >= 0 && col < 4) ? col : -1;

    if (preferred >= 0 && s->held[preferred] < 0) {
        return preferred;
    }
    for (i = 0; i < 4; i++) {
        if (s->held[i] < 0) {
            return i;
        }
    }
    return -1;
}

/* Find the DOWN slot currently holding a key at (row, col), or -1. */
static int sc6530_keypad_held_slot(Sc6530KeypadState *s, int row, int col)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (s->held[i] == row * 8 + col) {
            return i;
        }
    }
    return -1;
}

/* Matrix press edge: latch DOWN slot + the key's row byte. */
static void sc6530_keypad_press(Sc6530KeypadState *s, const Sc6530KeyMapEntry *m)
{
    int slot = sc6530_keypad_find_slot(s, m->col);
    uint32_t byte;
    int i;

    if (slot < 0) {
        qemu_log("sc6530_keypad: %s DOWN dropped (all 4 edge slots busy)\n",
                 m->name);
        return;
    }
    if (sc6530_keypad_held_slot(s, m->row, m->col) >= 0) {
        qemu_log("sc6530_keypad: %s DOWN ignored (already held)\n", m->name);
        return;
    }

    s->held[slot] = m->row * 8 + m->col;
    s->int_raw |= 1u << slot;
    /* Row byte: bits 6:4 = row, bits 2:0 = col -> k = row*8+col in the
     * firmware decode ((byte & 0x70) >> 1 | (byte & 7)). */
    byte = ((uint32_t)m->row << 4) | ((uint32_t)m->col & 7u);
    for (i = 0; i < 4; i++) {
        if ((slot & 3) == i) {
            s->key_status &= ~(0xffu << (i * 8));
            s->key_status |= byte << (i * 8);
        }
    }

    qemu_log("sc6530_keypad: %s DOWN (0x%02x) row=%d col=%d slot=%d "
             "int_raw=0x%02x key_status=0x%08x\n",
             m->name, m->code, m->row, m->col, slot,
             s->int_raw & SC6530_KPD_EDGE_ALL, s->key_status);
}

/* Matrix release edge: latch the release bit (slot + 4) of the held key.
 * The row byte stays in key_status - the firmware decodes the release
 * from the SAME byte ((i & 3) wrap in keypad_decode_for_test). */
static void sc6530_keypad_release(Sc6530KeypadState *s,
                                  const Sc6530KeyMapEntry *m)
{
    int slot = sc6530_keypad_held_slot(s, m->row, m->col);

    if (slot < 0) {
        qemu_log("sc6530_keypad: %s UP ignored (not held)\n", m->name);
        return;
    }

    s->held[slot] = -1;
    s->int_raw |= 1u << (slot + 4);

    qemu_log("sc6530_keypad: %s UP (0x%02x) row=%d col=%d slot=%d "
             "int_raw=0x%02x key_status=0x%08x\n",
             m->name, m->code, m->row, m->col, slot,
             s->int_raw & SC6530_KPD_EDGE_ALL, s->key_status);
}

/* EIC END key: raise/lower bit 3 of EIC_DBNC_DATA in the ADI's ANA bank
 * via the side-effect hook (the guest reads it through the ADI mailbox). */
static void sc6530_keypad_end(Sc6530KeypadState *s, bool held)
{
    if (held == s->end_held) {
        return;   /* no level change (e.g. repeated down) */
    }
    s->end_held = held;

    if (!s->adi) {
        qemu_log("sc6530_keypad: END %s dropped (no 'adi' link - EIC "
                 "unreachable)\n", held ? "DOWN" : "UP");
        return;
    }
    sc6530_adi_set_eic_pb(s->adi, held);
    qemu_log("sc6530_keypad: END %s (EIC_DBNC_DATA 0x82001900 bit 3 -> "
             "ANA hook)\n", held ? "DOWN" : "UP");
}

/* ---------------------------------------------------------------------- */
/* Generic input layer (template: hw/input/ps2.c)                         */
/* ---------------------------------------------------------------------- */

static void sc6530_keypad_event(DeviceState *dev, QemuConsole *src,
                                QemuInputEvent *evt)
{
    Sc6530KeypadState *s = SC6530_KEYPAD(dev);
    const Sc6530KeyMapEntry *m;

    assert(evt->type == INPUT_EVENT_KIND_KEY);
    m = sc6530_keymap_lookup(evt->key.key);
    if (!m) {
        qemu_log("sc6530_keypad: unmapped key lnx=%u %s\n", evt->key.key,
                 evt->key.down ? "down" : "up");
        return;
    }
    if (m->row < 0) {
        sc6530_keypad_end(s, evt->key.down);
    } else if (evt->key.down) {
        sc6530_keypad_press(s, m);
    } else {
        sc6530_keypad_release(s, m);
    }
}

static const QemuInputHandler sc6530_keypad_handler = {
    .name  = "SC6530 Keypad",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = sc6530_keypad_event,
};

/* ---------------------------------------------------------------------- */
/* Matrix MMIO                                                            */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_keypad_read(void *opaque, hwaddr offset,
                                   unsigned size)
{
    Sc6530KeypadState *s = opaque;

    switch (offset) {
    case SC6530_KPD_CTRL:
        return s->ctrl;
    case SC6530_KPD_INT_EN:
        return s->int_en;
    case SC6530_KPD_INT_RAW:
        return s->int_raw & SC6530_KPD_EDGE_ALL;
    case SC6530_KPD_INT_MASK:
        return s->int_mask;
    case SC6530_KPD_DUMMY:
        return s->dummy;
    case SC6530_KPD_POLARITY:
        return s->polarity;
    case SC6530_KPD_DEBOUNCE:
        return s->debounce;
    case SC6530_KPD_LONG_KEY:
        return s->long_key;
    case SC6530_KPD_SLEEP_CNT:
        return s->sleep_cnt;
    case SC6530_KPD_CLK_DIVIDE:
        return s->clk_divide;
    case SC6530_KPD_KEY_STATUS:
        return s->key_status;
    case SC6530_KPD_SLEEP_STAT:
        return s->sleep_stat;
    case SC6530_KPD_DBG_STAT1:
        return s->dbg_stat1;
    case SC6530_KPD_DBG_STAT2:
        return s->dbg_stat2;
    default:
        return 0;
    }
}

static void sc6530_keypad_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    Sc6530KeypadState *s = opaque;

    switch (offset) {
    case SC6530_KPD_CTRL:
        s->ctrl = value;
        break;
    case SC6530_KPD_INT_EN:
        s->int_en = value;
        break;
    case SC6530_KPD_INT_RAW:
        /* RO latch: absorb (the controller owns it). */
        break;
    case SC6530_KPD_INT_MASK:
        s->int_mask = value;
        break;
    case SC6530_KPD_INT_CLR:
        /* Edge ACK: the firmware clears with int_clr = 0xfff after every
         * read (drivers/keypad.c keypad_poll). Any write clears all
         * latched edges. The status row bytes persist (see header). */
        if (s->int_raw) {
            qemu_log("sc6530_keypad: int_clr write val=0x%" PRIx64
                     " clears int_raw=0x%02x\n", value,
                     s->int_raw & SC6530_KPD_EDGE_ALL);
        }
        s->int_raw = 0;
        break;
    case SC6530_KPD_DUMMY:
        s->dummy = value;
        break;
    case SC6530_KPD_POLARITY:
        s->polarity = value;
        break;
    case SC6530_KPD_DEBOUNCE:
        s->debounce = value;
        break;
    case SC6530_KPD_LONG_KEY:
        s->long_key = value;
        break;
    case SC6530_KPD_SLEEP_CNT:
        s->sleep_cnt = value;
        break;
    case SC6530_KPD_CLK_DIVIDE:
        s->clk_divide = value;
        break;
    case SC6530_KPD_KEY_STATUS:
        /* RO (controller status): absorb. */
        break;
    case SC6530_KPD_SLEEP_STAT:
        s->sleep_stat = value;
        break;
    case SC6530_KPD_DBG_STAT1:
        s->dbg_stat1 = value;
        break;
    case SC6530_KPD_DBG_STAT2:
        s->dbg_stat2 = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps sc6530_keypad_ops = {
    .read  = sc6530_keypad_read,
    .write = sc6530_keypad_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* SysBus device                                                          */
/* ---------------------------------------------------------------------- */

static bool sc6530_keypad_get_hold_end(Object *obj, Error **errp)
{
    return SC6530_KEYPAD(obj)->hold_end;
}

static void sc6530_keypad_set_hold_end(Object *obj, bool value, Error **errp)
{
    SC6530_KEYPAD(obj)->hold_end = value;
}

static void sc6530_keypad_hold_end_cb(void *opaque)
{
    Sc6530KeypadState *s = opaque;

    qemu_log("sc6530_keypad: 2-second hold-end timer expired -> releasing END key\n");
    sc6530_keypad_end(s, false);
}

static void sc6530_keypad_reset(DeviceState *dev)
{
    Sc6530KeypadState *s = SC6530_KEYPAD(dev);
    int i;

    s->ctrl = 0;
    s->int_en = 0;
    s->int_mask = 0;
    s->polarity = 0;
    s->debounce = 0;
    s->long_key = 0;
    s->sleep_cnt = 0;
    s->clk_divide = 0;
    s->dummy = 0;
    s->sleep_stat = 0;
    s->dbg_stat1 = 0;
    s->dbg_stat2 = 0;
    s->int_raw = 0;
    s->key_status = 0;
    for (i = 0; i < 4; i++) {
        s->held[i] = -1;
    }
    s->end_held = false;
    if (s->hold_end_timer) {
        timer_del(s->hold_end_timer);
    }
    if (s->hold_end) {
        sc6530_keypad_end(s, true);
        if (s->hold_end_timer) {
            timer_mod(s->hold_end_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      2 * NANOSECONDS_PER_SECOND);
        }
    } else if (s->adi) {
        sc6530_adi_set_eic_pb(s->adi, false);
    }
}

static void sc6530_keypad_init(Object *obj)
{
    Sc6530KeypadState *s = SC6530_KEYPAD(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->matrix_iomem, obj, &sc6530_keypad_ops, s,
                          "sc6530-keypad", SC6530_KEYPAD_SIZE);
    sysbus_init_mmio(sbd, &s->matrix_iomem);

    /* Link to the ADI device (wired by hw/arm/b310e.c): the EIC END key
     * raises/lowers bit 3 of EIC_DBNC_DATA inside the ADI's ANA bank,
     * which is the register the guest's mailbox read observes. */
    object_property_add_link(obj, "adi", TYPE_SC6530_ADI,
                             (Object **)&s->adi,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
    object_property_add_bool(obj, "hold-end",
                             sc6530_keypad_get_hold_end,
                             sc6530_keypad_set_hold_end);
}

static void sc6530_keypad_realize(DeviceState *dev, Error **errp)
{
    Sc6530KeypadState *s = SC6530_KEYPAD(dev);

    s->hold_end_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                     sc6530_keypad_hold_end_cb, s);

    /* Register with the QEMU generic input layer: sendkey /
     * input-send-event deliveries land in sc6530_keypad_event. */
    s->hs = qemu_input_handler_register(dev, &sc6530_keypad_handler);
}

static void sc6530_keypad_unrealize(DeviceState *dev)
{
    Sc6530KeypadState *s = SC6530_KEYPAD(dev);

    if (s->hold_end_timer) {
        timer_free(s->hold_end_timer);
        s->hold_end_timer = NULL;
    }
    g_clear_pointer(&s->hs, qemu_input_handler_unregister);
}

static const VMStateDescription sc6530_keypad_vmsd = {
    .name = "sc6530_keypad",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, Sc6530KeypadState),
        VMSTATE_UINT32(int_en, Sc6530KeypadState),
        VMSTATE_UINT32(int_mask, Sc6530KeypadState),
        VMSTATE_UINT32(polarity, Sc6530KeypadState),
        VMSTATE_UINT32(debounce, Sc6530KeypadState),
        VMSTATE_UINT32(long_key, Sc6530KeypadState),
        VMSTATE_UINT32(sleep_cnt, Sc6530KeypadState),
        VMSTATE_UINT32(clk_divide, Sc6530KeypadState),
        VMSTATE_UINT32(dummy, Sc6530KeypadState),
        VMSTATE_UINT32(sleep_stat, Sc6530KeypadState),
        VMSTATE_UINT32(dbg_stat1, Sc6530KeypadState),
        VMSTATE_UINT32(dbg_stat2, Sc6530KeypadState),
        VMSTATE_UINT32(int_raw, Sc6530KeypadState),
        VMSTATE_UINT32(key_status, Sc6530KeypadState),
        VMSTATE_INT32_ARRAY(held, Sc6530KeypadState, 4),
        VMSTATE_BOOL(end_held, Sc6530KeypadState),
        VMSTATE_END_OF_LIST()
    }
};

static void sc6530_keypad_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Spreadtrum SC6530 keypad matrix + EIC END key";
    dc->realize = sc6530_keypad_realize;
    dc->unrealize = sc6530_keypad_unrealize;
    device_class_set_legacy_reset(dc, sc6530_keypad_reset);
    dc->vmsd = &sc6530_keypad_vmsd;
}

static const TypeInfo sc6530_keypad_info = {
    .name          = TYPE_SC6530_KEYPAD,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530KeypadState),
    .instance_init = sc6530_keypad_init,
    .class_init    = sc6530_keypad_class_init,
};

static void sc6530_keypad_register_types(void)
{
    type_register_static(&sc6530_keypad_info);
}

type_init(sc6530_keypad_register_types)
