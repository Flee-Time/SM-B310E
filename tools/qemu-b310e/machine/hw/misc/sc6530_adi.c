/*
 * Spreadtrum SC6530C ADI mailbox + ANA analog register bank + VBC audio
 * FIFO region (B310E-OS QEMU machine).
 *
 * Todo 15 of .omo/plans/b310e-qemu-machine.md (Wave 3) - the "audio
 * observatory": every ANA/ADI/VBC access is traced with the guest PC so
 * the Wave-6 diff can capture the stock OS's codec power ladder and DAC
 * path (docs/audio-dsp-protocol.md "Cross-map", .omo/plans/
 * b310e-audio-hal.md section 2/3).
 *
 * Regions (all mapped by hw/arm/b310e.c at B310E_REGION_PRIORITY, above
 * the todo-12 catch-all):
 *
 *   mailbox 0x82000000, 0x1000 - the ADI ARM mailbox (SDK adi_reg_v5.h:
 *                                ADI_ARM_RD_CMD +0x18, ADI_ARM_RD_DATA
 *                                +0x1C, ADI_ARM_STS +0x20)
 *   ANA     0x82001000, 0x2000 - the analog-die register bank (8 KiB
 *                                covers the codec regs from the audio-hal
 *                                section-3 table + the watchdog
 *                                0x82001480 + the EIC 0x82001900; the
 *                                todo-12 aux device deliberately does NOT
 *                                map the watchdog - it lives here only)
 *   VBC     0x82003000, 0x100  - the VBC audio FIFO block (base
 *                                ARM_VBC_BASE, rb:47; FIFO ctl
 *                                VBDABUFFDTA +0x18, VBDAL/VBDAR +0x0/+0x4).
 *                                A SEPARATE region: a single 0x2000 ANA
 *                                region would end at 0x82002fff and MISS
 *                                the VBC base (plan todo 15).
 *
 * ADI mailbox protocol (proven on the dump: byte-identical helpers at
 * 0x302B6 read / 0x3034E write, cross-mapped to SDK adi_phy_v5.c
 * ADI_Analogdie_reg_read / ADI_Analogdie_reg_write):
 *
 *   READ: poll ADI_ARM_STS bit 8 (FIFO empty) == 1 -> write
 *         ADI_ARM_RD_CMD = (addr & 0xFFF) -> poll ADI_ARM_RD_DATA bit 31
 *         (busy) == 0 -> assert (rd_data & 0x1FFF0000) ==
 *         ((addr & 0xFFF) << 16) -> return rd_data & 0xFFFF.
 *         ADI_ARM_RD_DATA is [31]=busy, [28:16]=index echo, [15:0]=data.
 *   WRITE: poll ADI_ARM_STS bit 9 (FIFO full) == 0 -> DIRECT 32-bit store
 *         to the ANA address (no mailbox command on this chip - plan
 *         section 2, docs/audio-dsp-protocol.md "Cross-map R1").
 *
 *   The model completes every mailbox operation instantly:
 *     - ADI_ARM_STS returns BIT_8 (FIFO empty) and never sets BIT_9
 *       (FIFO full): 0x00000100. NOTE this is a deliberate deviation
 *       from the todo text ("return 0 (idle)") - the SDK macros are
 *       ADI_STS_FIFO_EMPTY_MASK = BIT_8 / ADI_STS_FIFO_FULL_MASK = BIT_9
 *       (adi_reg_v5.h:60-61) and BOTH helper poll loops wait for bit 8
 *       SET (adi_phy_v5.c:124/204 `while (ADI_FIFO_IS_EMPTY == 0)` and
 *       :186 `if (ADI_FIFO_IS_FULL == 0) break`); the dump disasm
 *       confirms (`lsls r1,#23; bpl loop` = loop while bit 8 clear).
 *       Returning 0 would spin the stock read helper at 0x302B6 AND
 *       os.bin's _start ADI wait (arch/start.s `tst #0x100; beq` - the
 *       exact poll the todo-12 benign table answered with 0x100, and
 *       which it flagged "superseded by todo 15's ADI model"). 0x100
 *       satisfies every known poll loop (todo-12 evidence: boot=ours
 *       never left _start+0x44 without it).
 *     - ADI_ARM_RD_DATA returns (rd_index << 16) | (ana value & 0xFFFF)
 *       with bit 31 (busy) clear: the index echo (0x1FFF0000 mask)
 *       always matches the last ADI_ARM_RD_CMD write and the poll
 *       terminates on the first read.
 *     - ADI_ARM_RD_CMD stores value & 0xFFF (the dump writes the raw
 *       index; the SDK ORs ADI_ARMREG_FLAG_MASK - masked off here).
 *
 * Audio trace events (sc6530_ana_read/sc6530_ana_write, enabled at
 * runtime with --trace "sc6530_ana_*"):
 *   - every ANA/VBC access traces its own address, value and guest PC
 *     (pc = current_cpu ? regs[15] : 0, arm PC);
 *   - the ADI mailbox read-data access (0x8200001C) traces the RESOLVED
 *     ANA register address 0x82001000 + rd_index - NOT the MMIO offset
 *     0x8200001c - so the read-trace address is the register the guest
 *     actually observed (the index-echo state machine feeds it);
 *   - the ADI_ARM_RD_CMD write and ADI_ARM_STS read trace their MMIO
 *     addresses (the mailbox command sequence itself is part of the
 *     observatory record).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/bitops.h"
#include "hw/core/sysbus.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"
#include "trace.h"

#define TYPE_SC6530_ADI "sc6530_adi"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530AdiState, SC6530_ADI)

/* Exported for todo 17's sc6530_keypad (the EIC END-key hook, see below):
 * raises/lowers bit 3 of EIC_DBNC_DATA inside the ANA bank. */
void sc6530_adi_set_eic_pb(Object *adi_obj, bool held);

/* ---------------------------------------------------------------------- */
/* Region geometry                                                        */
/* ---------------------------------------------------------------------- */

#define SC6530_ADI_MAILBOX_BASE  0x82000000ULL
#define SC6530_ADI_MAILBOX_SIZE  0x1000
#define SC6530_ADI_ANA_BASE      0x82001000ULL
#define SC6530_ADI_ANA_SIZE      0x2000   /* 8 KiB: codec + WDG + EIC */
#define SC6530_ADI_VBC_BASE      0x82003000ULL
#define SC6530_ADI_VBC_SIZE      0x100

/* Mailbox register offsets (SDK adi_reg_v5.h:40-42). */
#define SC6530_ADI_RD_CMD_OFF    0x18
#define SC6530_ADI_RD_DATA_OFF   0x1C
#define SC6530_ADI_STS_OFF       0x20

/* STS bits: ADI_STS_FIFO_EMPTY_MASK = BIT_8, ADI_STS_FIFO_FULL_MASK =
 * BIT_9 (adi_reg_v5.h:60-61). Idle = FIFO empty (1) + not full (0). */
#define SC6530_ADI_STS_IDLE      0x00000100u

/* EIC registers inside the ANA bank (todo 17's sc6530_keypad hook):
 * EIC_DBNC_DATA @ 0x82001900 bit 3 = the B310E hangup/END key level
 * (fpdoom keypad_read_pb, drivers/keypad.c), EIC_DBNC_DMSK @ 0x82001904
 * bit 3 = the channel-unmask the guest writes in keypad_eic_enable. */
#define SC6530_ADI_EIC_DATA_OFF  0x900
#define SC6530_ADI_EIC_DMSK_OFF  0x904
#define SC6530_ADI_EIC_PB_CH     3

/* ADI_ARM_RD_DATA fields (adi_reg_v5.h:55-57). */
#define SC6530_ADI_RD_BUSY_MASK  (1u << 31)
#define SC6530_ADI_RD_ADDR_MASK  0x1FFF0000u   /* [28:16] index echo */
#define SC6530_ADI_RD_DATA_MASK  0x0000FFFFu   /* [15:0] analog value */

/* Benign-ready entry (W4-20b os.bin smoke, 2026-08-24): the guest battery
 * fuel gauge (drivers/battery.c bat_adc_convert_ch) polls BAT_ADC_STATUS
 * (ANA offset 0x6DC = 0x820016dc) bit 0 = conversion-done with a
 * 1M-iteration budget. The emulator has no analog die, so the done bit
 * never sets and every conversion spins its full budget (~0.85 s per read
 * under TCG -> the banner frame rate collapses to ~1 fps; the guest
 * degrades gracefully via the timeout sentinel, but slowly). Answer
 * bit 0 = done: the wait terminates on the first read and the guest then
 * reads BAT_ADC_RESULT (0x820016cc, still 0) -> battery shows 0 mV / 0%
 * (truthful - there is no battery in the emulator). Covers BOTH read
 * paths: the mailbox RD_DATA resolve AND direct ANA-bank reads (the stock
 * OS's adc_phy_v5.c uses the mailbox; direct reads are covered for free). */
#define SC6530_ADI_ADC_STATUS_OFF 0x6DCu

/* ---------------------------------------------------------------------- */
/* Device state                                                           */
/* ---------------------------------------------------------------------- */

struct Sc6530AdiState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion mailbox_iomem;   /* 0x82000000 */
    MemoryRegion ana_iomem;       /* 0x82001000 */
    MemoryRegion vbc_iomem;       /* 0x82003000 */

    uint32_t mailbox_regs[SC6530_ADI_MAILBOX_SIZE / 4];
    uint32_t ana_regs[SC6530_ADI_ANA_SIZE / 4];
    uint32_t vbc_regs[SC6530_ADI_VBC_SIZE / 4];

    /* Index-echo state: the last ADI_ARM_RD_CMD write (addr & 0xFFF). */
    uint32_t rd_index;
    /* Physical EIC power-button level (bit 3). Kept separate from the
     * ana_regs EIC_DATA word so a key held from BEFORE the guest's DMSK
     * unmask is not lost: the debounce-mask gate is applied at READ time
     * in sc6530_adi_ana_effective, not when the level is set. */
    uint32_t eic_pb_phys;
};

/* Effective ANA-bank word: the stored value with the benign-ready answers
 * applied (see the SC6530_ADI_ADC_STATUS_OFF entry above). */
static uint32_t sc6530_adi_ana_effective(const Sc6530AdiState *s,
                                         hwaddr offset)
{
    uint32_t word = s->ana_regs[offset / 4];

    if (offset == SC6530_ADI_ADC_STATUS_OFF) {
        word |= 1u;   /* ADC conversion done */
    }
    if (offset == SC6530_ADI_EIC_DATA_OFF) {
        /* Physical EIC level is the ground truth (the debounce-mask is a
         * config the guest may not touch for the power-button channel). */
        word |= (s->eic_pb_phys & 0xffffu);
    }
    return word;
}

/* ---------------------------------------------------------------------- */
/* Shared store+echo helpers (size-aware byte/word access on the arrays)  */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_adi_regs_read(const uint32_t *regs, hwaddr offset,
                                     unsigned size)
{
    uint32_t word = regs[offset / 4];

    return extract32(word, (offset % 4) * 8, size * 8);
}

static void sc6530_adi_regs_write(uint32_t *regs, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    uint32_t word = regs[offset / 4];
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (offset % 4) * 8;

    regs[offset / 4] = (word & ~(mask << shift)) |
                       ((uint32_t)value & mask) << shift;
}

static uint32_t sc6530_adi_guest_pc(void)
{
    CPUState *cs = current_cpu;

    if (cs) {
        return ARM_CPU(cs)->env.regs[15];
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Mailbox bank: the ADI ARM mailbox (0x82000000).                        */
/*  - +0x18 RD_CMD write: store addr & 0xFFF as the read index.          */
/*  - +0x1C RD_DATA read: (index << 16) | (ANA value & 0xFFFF), busy bit */
/*    clear - the index echo always matches, polls terminate instantly.  */
/*    The read-trace event carries the RESOLVED ANA address              */
/*    (0x82001000 + index), not this MMIO offset.                        */
/*  - +0x20 STS read: BIT_8 (FIFO empty) set, BIT_9 (FIFO full) clear =  */
/*    0x100 - every guest poll loop terminates (see the header comment). */
/*  - all other offsets: store+echo so RMW chains stay stable.           */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_adi_mailbox_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    Sc6530AdiState *s = opaque;
    uint64_t val;

    switch (offset) {
    case SC6530_ADI_RD_DATA_OFF:
        /* [31]=busy(0), [28:16]=index echo, [15:0]=ANA register value.
         * The ANA register is resolved from the LAST RD_CMD write; the
         * trace addr is that resolved ANA register, not 0x8200001c. */
        val = ((uint64_t)s->rd_index << 16) |
              (sc6530_adi_ana_effective(s, s->rd_index) &
               SC6530_ADI_RD_DATA_MASK);
        trace_sc6530_ana_read(SC6530_ADI_ANA_BASE + s->rd_index, val,
                              sc6530_adi_guest_pc());
        return val;
    case SC6530_ADI_STS_OFF:
        /* FIFO empty (ready) + not full: both helper poll loops exit. */
        val = SC6530_ADI_STS_IDLE;
        trace_sc6530_ana_read(SC6530_ADI_MAILBOX_BASE + offset, val,
                              sc6530_adi_guest_pc());
        return val;
    default:
        val = sc6530_adi_regs_read(s->mailbox_regs, offset, size);
        trace_sc6530_ana_read(SC6530_ADI_MAILBOX_BASE + offset, val,
                              sc6530_adi_guest_pc());
        return val;
    }
}

static void sc6530_adi_mailbox_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    Sc6530AdiState *s = opaque;

    switch (offset) {
    case SC6530_ADI_RD_CMD_OFF:
        /* Read-command: the register index is addr & 0xFFF (the dump's
         * ADI_Analogdie_reg_read writes exactly that; the SDK ORs
         * ADI_ARMREG_FLAG_MASK - masked off). Feeds the read-data
         * index echo + the resolved read-trace address. */
        s->rd_index = (uint32_t)value & 0xFFF;
        break;
    default:
        sc6530_adi_regs_write(s->mailbox_regs, offset, value, size);
        break;
    }
    trace_sc6530_ana_write(SC6530_ADI_MAILBOX_BASE + offset, value,
                           sc6530_adi_guest_pc());
}

static const MemoryRegionOps sc6530_adi_mailbox_ops = {
    .read  = sc6530_adi_mailbox_read,
    .write = sc6530_adi_mailbox_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* ANA bank: store+echo + trace. Every read/write of a codec, WDG or EIC  */
/* register lands in the trace with the full guest address + PC (the      */
/* audio observatory - Wave-6 diff input).                                */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_adi_ana_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    Sc6530AdiState *s = opaque;
    uint32_t word = sc6530_adi_ana_effective(s, offset);
    uint64_t val = extract32(word, (offset % 4) * 8, size * 8);

    trace_sc6530_ana_read(SC6530_ADI_ANA_BASE + offset, val,
                          sc6530_adi_guest_pc());
    return val;
}

static void sc6530_adi_ana_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    Sc6530AdiState *s = opaque;

    sc6530_adi_regs_write(s->ana_regs, offset, value, size);
    trace_sc6530_ana_write(SC6530_ADI_ANA_BASE + offset, value,
                           sc6530_adi_guest_pc());
}

static const MemoryRegionOps sc6530_adi_ana_ops = {
    .read  = sc6530_adi_ana_read,
    .write = sc6530_adi_ana_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* VBC bank: store+echo + trace (base ARM_VBC_BASE 0x82003000; FIFO ctl   */
/* VBDABUFFDTA +0x18, VBDAL/VBDAR +0x0/+0x4 - docs/audio-dsp-protocol.md  */
/* "Cross-map" row 12).                                                   */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_adi_vbc_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    Sc6530AdiState *s = opaque;
    uint64_t val = sc6530_adi_regs_read(s->vbc_regs, offset, size);

    trace_sc6530_ana_read(SC6530_ADI_VBC_BASE + offset, val,
                          sc6530_adi_guest_pc());
    return val;
}

static void sc6530_adi_vbc_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    Sc6530AdiState *s = opaque;

    sc6530_adi_regs_write(s->vbc_regs, offset, value, size);
    trace_sc6530_ana_write(SC6530_ADI_VBC_BASE + offset, value,
                           sc6530_adi_guest_pc());
}

static const MemoryRegionOps sc6530_adi_vbc_ops = {
    .read  = sc6530_adi_vbc_read,
    .write = sc6530_adi_vbc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------------------------- */
/* EIC power-button hook (todo 17's sc6530_keypad)                        */
/*                                                                        */
/* The B310E hangup/END key is the EIC power button: bit 3 of            */
/* EIC_DBNC_DATA @ 0x82001900, read by the guest EXCLUSIVELY through     */
/* the ADI mailbox (keypad_adi_read in drivers/keypad.c: RD_CMD          */
/* 0x82000018 = 0x900, then RD_DATA 0x8200001c), which resolves from     */
/* THIS device's ana_regs[] array - the mailbox never consults the       */
/* address space, so an MMIO overlay at 0x82001900 would be invisible    */
/* to the guest. sc6530_keypad therefore holds a QOM link to this        */
/* device and calls this hook to raise/lower the END level inside the    */
/* ANA bank. The guest's DMSK unmask (keypad_eic_enable writes           */
/* EIC_DBNC_DMSK |= 1<<3) gates visibility, mirroring the real          */
/* debounce-mask semantics. The write emits the sc6530_ana_write trace   */
/* event so END toggles show up in the audio-observatory trace.          */
/* ---------------------------------------------------------------------- */

void sc6530_adi_set_eic_pb(Object *adi_obj, bool held)
{
    Sc6530AdiState *s = SC6530_ADI(adi_obj);
    uint32_t bit = 1u << SC6530_ADI_EIC_PB_CH;
    uint32_t phys = s->eic_pb_phys;

    if (held) {
        phys |= bit;
    } else {
        phys &= ~bit;
    }
    if (phys != s->eic_pb_phys) {
        s->eic_pb_phys = phys;
        qemu_log("sc6530_adi: EIC PB phys %s (bit3, DMSK-gated at read)\n",
                 held ? "held" : "released");
        trace_sc6530_ana_write(SC6530_ADI_ANA_BASE + SC6530_ADI_EIC_DATA_OFF,
                               phys, sc6530_adi_guest_pc());
    }
}

/* ---------------------------------------------------------------------- */
/* SysBus device                                                          */
/* ---------------------------------------------------------------------- */

static void sc6530_adi_reset(DeviceState *dev)
{
    Sc6530AdiState *s = SC6530_ADI(dev);

    memset(s->mailbox_regs, 0, sizeof(s->mailbox_regs));
    memset(s->ana_regs, 0, sizeof(s->ana_regs));
    memset(s->vbc_regs, 0, sizeof(s->vbc_regs));
    s->rd_index = 0;

    /* Initialize RTC registers to a valid date/time so stock OS RTC validation passes.
     * RTC_SEC: 0x82001620, RTC_MIN: 0x82001624, RTC_HOUR: 0x82001628,
     * RTC_DAY: 0x8200162c, RTC_MON: 0x82001630, RTC_YEAR: 0x82001634
     */
    s->ana_regs[(0x620) / 4] = 0;    /* sec = 0 */
    s->ana_regs[(0x624) / 4] = 0;    /* min = 0 */
    s->ana_regs[(0x628) / 4] = 12;   /* hour = 12 */
    s->ana_regs[(0x62c) / 4] = 1;    /* day = 1 */
    s->ana_regs[(0x630) / 4] = 1;    /* month = 1 */
    s->ana_regs[(0x634) / 4] = 2024; /* year = 2024 */
    /* eic_pb_phys is the KEYPAD's domain (its reset asserts/releases the
     * hold-end level) - the ADI reset must NOT zero it (the keypad reset
     * runs BEFORE this one, so zeroing here would wipe the hold). */
}

static void sc6530_adi_init(Object *obj)
{
    Sc6530AdiState *s = SC6530_ADI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mailbox_iomem, obj, &sc6530_adi_mailbox_ops,
                          s, "sc6530-adi-mailbox", SC6530_ADI_MAILBOX_SIZE);
    sysbus_init_mmio(sbd, &s->mailbox_iomem);

    memory_region_init_io(&s->ana_iomem, obj, &sc6530_adi_ana_ops, s,
                          "sc6530-adi-ana", SC6530_ADI_ANA_SIZE);
    sysbus_init_mmio(sbd, &s->ana_iomem);

    memory_region_init_io(&s->vbc_iomem, obj, &sc6530_adi_vbc_ops, s,
                          "sc6530-adi-vbc", SC6530_ADI_VBC_SIZE);
    sysbus_init_mmio(sbd, &s->vbc_iomem);
}

static void sc6530_adi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Spreadtrum SC6530 ADI mailbox + ANA analog bank + VBC";
    device_class_set_legacy_reset(dc, sc6530_adi_reset);
}

static const TypeInfo sc6530_adi_info = {
    .name          = TYPE_SC6530_ADI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530AdiState),
    .instance_init = sc6530_adi_init,
    .class_init    = sc6530_adi_class_init,
};

static void sc6530_adi_register_types(void)
{
    type_register_static(&sc6530_adi_info);
}

type_init(sc6530_adi_register_types)
