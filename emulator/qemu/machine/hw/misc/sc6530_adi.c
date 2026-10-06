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
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qemu/audio.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/cpu.h"
#include "qapi/error.h"
#include "system/system.h"
#include "system/runstate.h"
#include "target/arm/cpu.h"
#include "trace.h"

#define TYPE_SC6530_ADI "sc6530_adi"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530AdiState, SC6530_ADI)

/* Exported for todo 17's sc6530_keypad (the EIC END-key hook, see below):
 * raises/lowers bit 3 of EIC_DBNC_DATA inside the ANA bank. */
void sc6530_adi_set_eic_pb(Object *adi_obj, bool held);
size_t sc6530_dma_request(Object *obj, hwaddr destination, size_t bytes);
bool sc6530_dsp_arm_audio_owned(Object *obj);

/* ---------------------------------------------------------------------- */
/* Region geometry                                                        */
/* ---------------------------------------------------------------------- */

#define SC6530_ADI_MAILBOX_BASE  0x82000000ULL
#define SC6530_ADI_MAILBOX_SIZE  0x1000
#define SC6530_ADI_ANA_BASE      0x82001000ULL
#define SC6530_ADI_ANA_SIZE      0x2000   /* 8 KiB: codec + WDG + EIC */
#define SC6530_ADI_VBC_BASE      0x82003000ULL
#define SC6530_ADI_VBC_SIZE      0x100
#define SC6530_DP_SIZE          0x100
#define VBC_BANK_FRAMES         160
#define VBC_PCM_FRAMES          8192
#define VBC_ENABLE             (1u << 15)
#define VBC_RAM_ACCESS         (1u << 10)
#define VBC_RAM_BANK           (1u << 9)

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

/* Conversion starts at ANA +0x680 and sets this done flag. Channel 5
 * returns battery-adc (default 900, about 3978 mV with the stock
 * calibration). Returning zero makes the stock OS power itself off. */
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
    MemoryRegion dp_iomem;        /* 0x8a002000 digital codec */

    uint32_t mailbox_regs[SC6530_ADI_MAILBOX_SIZE / 4];
    uint16_t battery_adc;
    bool charger_present;
    uint32_t ana_regs[SC6530_ADI_ANA_SIZE / 4];
    uint32_t vbc_regs[SC6530_ADI_VBC_SIZE / 4];
    uint32_t dp_regs[SC6530_DP_SIZE / 4];
    Object *dma;
    Object *dsp;
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    QEMUTimer *audio_timer;
    int64_t audio_deadline;
    Notifier audio_exit;
    qemu_irq analog_irq;
    QEMUTimer *rtc_timer;
    unsigned rate;
    unsigned play_bank;
    unsigned play_pos;
    unsigned write_pos[2][2];
    int16_t bank[2][2][VBC_BANK_FRAMES];
    uint8_t pcm[VBC_PCM_FRAMES * 4]; /* explicitly little-endian stereo */
    unsigned pcm_head;
    unsigned pcm_count;
    bool dma_filling;
    bool analog_audio_clock;

    /* Index-echo state: the last ADI_ARM_RD_CMD write (addr & 0xFFF). */
    uint32_t rd_index;
    /* Physical EIC power-button level (bit 3). Kept separate from the
     * ana_regs EIC_DATA word so a key held from BEFORE the guest's DMSK
     * unmask is not lost: the debounce-mask gate is applied at READ time
     * in sc6530_adi_ana_effective, not when the level is set. */
    uint32_t eic_pb_phys;
};

static uint32_t sc6530_analog_pending(const Sc6530AdiState *s)
{
    /* Stock IRQ context at 0x0422ca4c uses ANA +0x580 for masked
     * status and +0x588 for enables; RTC is analog source bit 2. */
    return (s->ana_regs[0x634 / 4] & s->ana_regs[0x630 / 4]) ? 4 : 0;
}

static void sc6530_analog_irq(Sc6530AdiState *s)
{
    qemu_set_irq(s->analog_irq,
                 (sc6530_analog_pending(s) & s->ana_regs[0x588 / 4]) != 0);
}

static void sc6530_rtc_tick(void *opaque)
{
    Sc6530AdiState *s = opaque;
    uint32_t *r = s->ana_regs;
    uint32_t raw = 1;
    if (++r[0x600 / 4] >= 60) {
        r[0x600 / 4] = 0;
        raw |= 2;
        if (++r[0x604 / 4] >= 60) {
            r[0x604 / 4] = 0;
            raw |= 4;
            if (++r[0x608 / 4] >= 24) {
                r[0x608 / 4] = 0;
                r[0x60c / 4]++;
                raw |= 8;
            }
        }
    }
    if (r[0x600 / 4] == r[0x620 / 4] &&
        r[0x604 / 4] == r[0x624 / 4] &&
        r[0x608 / 4] == r[0x628 / 4] &&
        r[0x60c / 4] == r[0x62c / 4]) {
        raw |= 16;
    }
    r[0x634 / 4] |= raw;
    sc6530_analog_irq(s);
    timer_mod(s->rtc_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
}

static unsigned sc6530_vbc_frames(Sc6530AdiState *s)
{
    /* NOR 0xb0740..0xb075a: DA size-1 occupies bits [15:8]. */
    return MIN(((s->vbc_regs[0x10 / 4] >> 8) & 0xff) + 1,
               VBC_BANK_FRAMES);
}


static void sc6530_audio_callback(void *opaque, int available)
{
    Sc6530AdiState *s = opaque;
    while (available >= 4 && s->pcm_count) {
        unsigned frames = MIN(s->pcm_count, VBC_PCM_FRAMES - s->pcm_head);
        unsigned bytes = MIN(frames * 4, (unsigned)available & ~3u);
        size_t written = audio_be_write(s->audio_be, s->voice,
                                        s->pcm + s->pcm_head * 4, bytes);
        if (!written) {
            break;
        }
        s->pcm_head = (s->pcm_head + written / 4) % VBC_PCM_FRAMES;
        s->pcm_count -= written / 4;
        available -= written;
    }
    if (!s->pcm_count && !(s->vbc_regs[0x18 / 4] & VBC_ENABLE)) {
        audio_be_set_active_out(s->audio_be, s->voice, false);
    }
}

static void sc6530_audio_open(Sc6530AdiState *s)
{
    struct audsettings settings = {
        .freq = s->rate, .nchannels = 2, .fmt = AUDIO_FORMAT_S16,
        .big_endian = false,
    };
    s->voice = audio_be_open_out(s->audio_be, s->voice, "sc6530.vbc",
                                 s, sc6530_audio_callback, &settings);
}

static void sc6530_audio_tick(void *opaque)
{
    Sc6530AdiState *s = opaque;
    unsigned frames = sc6530_vbc_frames(s);
    unsigned count = MIN(MAX(s->rate / 1000, 1u), frames - s->play_pos);
    uint32_t ctl = s->vbc_regs[0x18 / 4];
    bool owned = s->dsp && sc6530_dsp_arm_audio_owned(s->dsp);
    /* DAC_CTL bit15 enables the ramp controller; bit14 requests mute.
     * Stock normally plays with 0x8000 set and 0x4000 clear. */
    bool mute = !owned || !s->analog_audio_clock ||
                ((s->dp_regs[0x0c / 4] & 0xc000) == 0xc000);

    if (!(ctl & VBC_ENABLE)) {
        return;
    }
    if (!s->play_pos && owned && s->dma) {
        s->dma_filling = true;
        for (unsigned channel = 0; channel < 2; channel++) {
            if (ctl & (1u << (13 + channel))) {
                memset(s->bank[s->play_bank][channel], 0,
                       sizeof(s->bank[s->play_bank][channel]));
                s->write_pos[s->play_bank][channel] = 0;
                sc6530_dma_request(s->dma, SC6530_ADI_VBC_BASE + channel * 4,
                                   frames * 2);
            }
        }
        s->dma_filling = false;
    }
    for (unsigned i = 0; i < count; i++) {
        if (s->pcm_count < VBC_PCM_FRAMES) {
            unsigned tail = (s->pcm_head + s->pcm_count) % VBC_PCM_FRAMES;
            for (unsigned channel = 0; channel < 2; channel++) {
                int16_t sample = s->bank[s->play_bank][channel][s->play_pos];
                if (mute || !(s->dp_regs[0] & (1u << (channel * 2)))) {
                    sample = 0;
                }
                stw_le_p(s->pcm + tail * 4 + channel * 2, sample);
            }
            s->pcm_count++;
        }
        s->play_pos++;
    }
    if (s->play_pos == frames) {
        s->play_pos = 0;
        s->play_bank ^= 1;
        trace_sc6530_vbc_bank(s->play_bank, frames, s->pcm_count);
    }
    /* The DAC clock runs independently of host callback latency. Anchoring
     * every tick to "now" accumulates that latency and slows the guest's
     * stream, especially on Windows. Keep the deadline on the sample clock. */
    s->audio_deadline += (uint64_t)count * NANOSECONDS_PER_SECOND / s->rate;
    timer_mod(s->audio_timer, s->audio_deadline);
}

static uint64_t sc6530_dp_read(void *opaque, hwaddr offset, unsigned size)
{
    Sc6530AdiState *s = opaque;
    uint32_t value = s->dp_regs[offset / 4];
    return extract32(value, (offset & 3) * 8, size * 8);
}

static void sc6530_dp_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    Sc6530AdiState *s = opaque;
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1;
    uint32_t *word = &s->dp_regs[offset / 4];
    /* NOR DAC FS lookup at 0x8123e..: modes run from 96k (0) to 8k (10).
     * The historic custom driver used 9 for 8k; 9 actually selects 9600. */
    static const unsigned rates[] = {
        96000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 9600, 8000,
    };
    *word = (*word & ~(mask << shift)) | ((value & mask) << shift);
    if ((offset & ~3) == 0x0c) {
        unsigned mode = *word & 0xf;
        if (mode < ARRAY_SIZE(rates) && s->rate != rates[mode]) {
            s->rate = rates[mode];
            s->audio_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->pcm_head = s->pcm_count = 0;
            sc6530_audio_open(s);
            audio_be_set_active_out(s->audio_be, s->voice,
                                    !!(s->vbc_regs[0x18 / 4] & VBC_ENABLE));
        }
        trace_sc6530_vbc_rate(mode, s->rate);
    }
    trace_sc6530_ana_write(0x8a002000 + offset, value,
                           current_cpu ? ARM_CPU(current_cpu)->env.regs[15] : 0);
}

static const MemoryRegionOps sc6530_dp_ops = {
    .read = sc6530_dp_read,
    .write = sc6530_dp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* Effective ANA-bank word: the stored value with the benign-ready answers
 * applied (see the SC6530_ADI_ADC_STATUS_OFF entry above). */
static uint32_t sc6530_adi_ana_effective(const Sc6530AdiState *s,
                                         hwaddr offset)
{
    offset &= ~3;
    uint32_t word = s->ana_regs[offset / 4];

    if (offset == 0x580) {
        return sc6530_analog_pending(s) & s->ana_regs[0x588 / 4];
    } else if (offset == 0x584) {
        return sc6530_analog_pending(s);
    } else if (offset == 0x63c) {
        return s->ana_regs[0x634 / 4] & s->ana_regs[0x630 / 4];
    } else if (offset == SC6530_ADI_EIC_DATA_OFF) {
        /* Physical EIC level is the ground truth (the debounce-mask is a
         * config the guest may not touch for the power-button channel). */
        word |= (s->eic_pb_phys & 0xffffu);
        /* e52q7a 0x663d4: logical EIC18 = analog channel2, active high.
         * External power cannot be written by the guest. Only polling
         * is modeled here; debounce/edge interrupts remain unsupported. */
        word &= ~(1u << 2);
        if (s->charger_present &&
            (s->ana_regs[SC6530_ADI_EIC_DMSK_OFF / 4] & (1u << 2))) {
            word |= 1u << 2;
        }
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
    unsigned aligned = offset & ~3;
    /* Stock 0x81562/0x8153e use write-one SET/CLEAR aliases. The audio
     * clock is independent of DMA: a missing clock leaves playback
     * counters advancing while the analog output remains silent. */
    if ((aligned == 0x440 || aligned == 0x444) &&
        ((value << ((offset & 3) * 8)) & 4)) {
        s->analog_audio_clock = aligned == 0x440;
    }
    if (aligned >= 0x610 && aligned <= 0x61c) {
        /* RTC time update -> counter, acknowledge bits 8..11. Stock
         * 0x36008 updates each field and waits for its ISR to clear ACK. */
        s->ana_regs[(aligned - 0x10) / 4] = s->ana_regs[aligned / 4];
        s->ana_regs[0x634 / 4] |= 1u << (8 + (aligned - 0x610) / 4);
    } else if (aligned >= 0x620 && aligned <= 0x62c) {
        s->ana_regs[0x634 / 4] |= 1u << (12 + (aligned - 0x620) / 4);
    } else if (aligned == 0x638) {
        s->ana_regs[0x634 / 4] &= ~(value << ((offset & 3) * 8));
        s->ana_regs[0x638 / 4] = 0;
    }
    sc6530_analog_irq(s);
    /* Stock 0x20ef6 samples channel 5 via ADC_CTL +0x680, channel +0x684,
     * result +0x6cc, clear +0x6d4 and raw status +0x6dc. Conversion is
     * immediate for now. The dump calibrates raw 950 as 4200 mV. */
    if ((offset & ~3) == 0x680 && (s->ana_regs[0x680 / 4] & 2)) {
        unsigned channel = s->ana_regs[0x684 / 4] & 0xf;
        s->ana_regs[0x6cc / 4] = channel == 5 ? s->battery_adc : 0;
        s->ana_regs[SC6530_ADI_ADC_STATUS_OFF / 4] |= 1;
        s->ana_regs[0x680 / 4] &= ~2u;
    } else if ((offset & ~3) == 0x6d4 && (value & 1)) {
        s->ana_regs[SC6530_ADI_ADC_STATUS_OFF / 4] &= ~1u;
    }
    trace_sc6530_ana_write(SC6530_ADI_ANA_BASE + offset, value,
                           sc6530_adi_guest_pc());
    /* e52q7a's shared ADI write helper saves its caller at SP+20.
     * Recording that return address distinguishes the live chip-specific
     * clock/codec routines from unused NOR implementations. */
    if (trace_event_get_state_backends(TRACE_SC6530_ANA_CALLER) &&
        current_cpu && sc6530_adi_guest_pc() == 0x3038a) {
        uint8_t caller[4];
        uint32_t sp = ARM_CPU(current_cpu)->env.regs[13];
        if (cpu_memory_rw_debug(current_cpu, sp + 20, caller, 4, false) == 0) {
            trace_sc6530_ana_caller(SC6530_ADI_ANA_BASE + offset, value,
                                   ldl_le_p(caller));
        }
    }
    /* Stock NOR 0x1a448..52 shuts down all supplies with SET1=0x1f,
     * SET0=0x3fff. Recognize that complete request, rather than treating
     * ordinary per-device LDO writes as a board power-off. Individual
     * rails, their delays and USB charger restart are not modeled. */
    if (aligned == 0x180 && (s->ana_regs[0x180 / 4] & 0x3fff) == 0x3fff &&
        (s->ana_regs[0x184 / 4] & 0x1f) == 0x1f) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
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
    uint32_t word = s->vbc_regs[offset / 4];
    uint64_t val;

    if ((offset & ~3) == 0x18 && (word & VBC_ENABLE)) {
        word = (word & ~VBC_RAM_BANK) | (s->play_bank << 9);
    }
    val = extract32(word, (offset & 3) * 8, size * 8);

    trace_sc6530_ana_read(SC6530_ADI_VBC_BASE + offset, val,
                          sc6530_adi_guest_pc());
    return val;
}

static void sc6530_adi_vbc_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    Sc6530AdiState *s = opaque;
    uint32_t old_ctl = s->vbc_regs[0x18 / 4];

    sc6530_adi_regs_write(s->vbc_regs, offset, value, size);
    if (offset == 0 || offset == 4) {
        unsigned channel = offset / 4;
        unsigned bank = s->dma_filling ? s->play_bank :
            (old_ctl & VBC_ENABLE) ? (s->play_bank ^ 1) :
            !!(old_ctl & VBC_RAM_BANK);
        if (s->dma_filling || (old_ctl & (VBC_ENABLE | VBC_RAM_ACCESS))) {
            unsigned *pos = &s->write_pos[bank][channel];
            s->bank[bank][channel][*pos] = (int16_t)value;
            *pos = (*pos + 1) % sc6530_vbc_frames(s);
        }
    } else if ((offset & ~3) == 0x18) {
        uint32_t ctl = s->vbc_regs[0x18 / 4];
        if (!(old_ctl & VBC_ENABLE) && (ctl & VBC_ENABLE)) {
            s->play_bank = s->play_pos = 0;
            s->pcm_head = s->pcm_count = 0;
            audio_be_set_active_out(s->audio_be, s->voice, true);
            s->audio_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            timer_mod(s->audio_timer, s->audio_deadline);
        } else if ((old_ctl & VBC_ENABLE) && !(ctl & VBC_ENABLE)) {
            timer_del(s->audio_timer);
            /* These frames have already left the emulated DAC. Let the
             * host consume them before deactivating its output voice. */
            if (!s->pcm_count) {
                audio_be_set_active_out(s->audio_be, s->voice, false);
            }
        }
        if ((old_ctl ^ ctl) & (VBC_RAM_BANK | VBC_RAM_ACCESS)) {
            unsigned bank = !!(ctl & VBC_RAM_BANK);
            s->write_pos[bank][0] = s->write_pos[bank][1] = 0;
        }
    } else if ((offset & ~3) == 0x10) {
        memset(s->write_pos, 0, sizeof(s->write_pos));
        s->play_pos = 0;
    }
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
    memset(s->dp_regs, 0, sizeof(s->dp_regs));
    memset(s->bank, 0, sizeof(s->bank));
    memset(s->write_pos, 0, sizeof(s->write_pos));
    s->pcm_head = s->pcm_count = s->play_pos = s->play_bank = 0;
    s->analog_audio_clock = false;
    timer_del(s->audio_timer);
    audio_be_set_active_out(s->audio_be, s->voice, false);
    s->rate = 8000;
    sc6530_audio_open(s);
    s->rd_index = 0;

    /* The SC6530 RTC has counters at +0x600, alarms at +0x620 and IRQ
     * enable/raw at +0x630/+0x634. There are no month/year registers. */
    sc6530_analog_irq(s);
    timer_mod(s->rtc_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    /* eic_pb_phys is the KEYPAD's domain (its reset asserts/releases the
     * hold-end level) - the ADI reset must NOT zero it (the keypad reset
     * runs BEFORE this one, so zeroing here would wipe the hold). */
}

static void sc6530_adi_charger_input(void *opaque, int pin, int level)
{
    Sc6530AdiState *s = opaque;
    s->charger_present = level != 0;
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

    memory_region_init_io(&s->dp_iomem, obj, &sc6530_dp_ops, s,
                          "sc6530-codec-dp", SC6530_DP_SIZE);
    sysbus_init_mmio(sbd, &s->dp_iomem);
    s->rate = 8000;
    s->audio_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sc6530_audio_tick, s);
    s->rtc_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, sc6530_rtc_tick, s);
    sysbus_init_irq(sbd, &s->analog_irq);
    object_property_add_link(obj, "dma", "sc6530_dma", &s->dma,
                             object_property_allow_set_link, OBJ_PROP_LINK_STRONG);
    object_property_add_link(obj, "dsp", "sc6530_dsp", &s->dsp,
                             object_property_allow_set_link, OBJ_PROP_LINK_STRONG);
    qdev_init_gpio_in_named(DEVICE(obj), sc6530_adi_charger_input,
                           "charger-input", 1);
}

static void sc6530_audio_exit(Notifier *notifier, void *data)
{
    Sc6530AdiState *s = container_of(notifier, Sc6530AdiState, audio_exit);
    /* Sysbus devices need not be unrealized on process exit. Close the last
     * voice explicitly so the WAV backend finalizes its RIFF/data lengths. */
    timer_del(s->audio_timer);
    timer_del(s->rtc_timer);
    audio_be_close_out(s->audio_be, s->voice);
    s->voice = NULL;
}

static void sc6530_adi_realize(DeviceState *dev, Error **errp)
{
    Sc6530AdiState *s = SC6530_ADI(dev);
    if (s->battery_adc > 1023) {
        error_setg(errp, "battery-adc must fit the 10-bit ADC (0..1023)");
        return;
    }
    if (!audio_be_check(&s->audio_be, errp)) {
        return;
    }
    sc6530_audio_open(s);
    if (!s->voice) {
        error_setg(errp, "SC6530 VBC could not open audio output");
        return;
    }
    s->audio_exit.notify = sc6530_audio_exit;
    qemu_add_exit_notifier(&s->audio_exit);
}

static void sc6530_adi_unrealize(DeviceState *dev)
{
    Sc6530AdiState *s = SC6530_ADI(dev);
    qemu_remove_exit_notifier(&s->audio_exit);
    timer_del(s->audio_timer);
    timer_del(s->rtc_timer);
    audio_be_close_out(s->audio_be, s->voice);
    s->voice = NULL;
}

static void sc6530_adi_finalize(Object *obj)
{
    timer_free(SC6530_ADI(obj)->audio_timer);
    timer_free(SC6530_ADI(obj)->rtc_timer);
}

static const Property sc6530_adi_properties[] = {
    DEFINE_PROP_UINT16("battery-adc", Sc6530AdiState, battery_adc, 900),
    DEFINE_PROP_BOOL("charger-present", Sc6530AdiState, charger_present, false),
    DEFINE_AUDIO_PROPERTIES(Sc6530AdiState, audio_be),
};

static void sc6530_adi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Spreadtrum SC6530 ADI mailbox + ANA analog bank + VBC";
    dc->realize = sc6530_adi_realize;
    dc->unrealize = sc6530_adi_unrealize;
    device_class_set_props(dc, sc6530_adi_properties);
    device_class_set_legacy_reset(dc, sc6530_adi_reset);
}

static const TypeInfo sc6530_adi_info = {
    .name          = TYPE_SC6530_ADI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530AdiState),
    .instance_init = sc6530_adi_init,
    .instance_finalize = sc6530_adi_finalize,
    .class_init    = sc6530_adi_class_init,
};

static void sc6530_adi_register_types(void)
{
    type_register_static(&sc6530_adi_info);
}

type_init(sc6530_adi_register_types)
