/*
 * Spreadtrum SC6530C DSP fake: share-memory handshake + APB mailbox
 * registers (B310E-OS QEMU machine).
 *
 * Todo 18 of .omo/plans/b310e-qemu-machine.md (Wave 4), implemented
 * STRICTLY per docs/audio-dsp-protocol.md "DSP-fake-spec" (sections 1-7).
 * The MOCOR SDK is NOT consulted - every register, semantic and handshake
 * step below is pinned by that spec (citations are given as [spec #x]).
 *
 * DESIGN (per spec section 1 + the todo text):
 * - The 128 KiB shared-memory region is ALREADY mapped by hw/arm/b310e.c
 *   at 0x10000000 + the alias 0x30000000 (b310e.dsp-sharemem / dsp-alias,
 *   both at B310E_REGION_PRIORITY). THIS DEVICE DOES NOT RE-MAP IT: it
 *   holds a QOM link "sharemem" to the region (wired by b310e.c) and
 *   reads/writes the RAM through memory_region_get_ram_ptr().
 * - To OBSERVE the guest's arm_control_status writes (the handshake is
 *   driven through the RAM, not through a register - spec 2a/3b) the
 *   device maps two 0x20-byte CONTROL WINDOWS at base+0 of BOTH candidate
 *   bases (0x10000000 and 0x30000000, one priority level above the RAM
 *   regions). The windows FORWARD every access to the same ram_ptr
 *   (store/load honestly, spec 2a) and additionally watch the control
 *   fields (arm_ctl +0, dl_offset +4, dl_block_size +6). This gives exact
 *   0->1 edge timing and the true guest PC for the trace (spec 6), with
 *   no timer and no second RAM allocation. Reads past the window are
 *   plain RAM (the block DATA at base+8 and the log ring).
 * - The 7 APB registers (spec 2c) are 4-byte subregions mapped OVER the
 *   todo-12 sc6530_aux APB bank (0x8b000000) at one priority level higher,
 *   so DSP accesses hit these handlers and aux keeps everything else
 *   (incl. the PA-power halves 0x8b000060/64/a0/a4). PERI_CTL0
 *   0x8b0001c4 (audio ownership, spec 5) is OWNED by this device.
 *
 * PERMISSIVE RULE (spec 4): UNKNOWN-5 timing -> answer DSP_READY=1
 * immediately on reset release, ack every poll with the next expected
 * status bit, never let the guest spin. UNKNOWN-7/UNKNOWN-N4
 * (audio_dsp_info / 0x02300xxx tokens, shared-mem codec handoff) -> LOG,
 * do NOT interpret. No register semantics are invented: anything not
 * pinned by the spec is routed to the UNKNOWN event + qemu_log.
 *
 * TRACE SURFACE (spec 6): single event sc6530_dsp_cmd(id, arg, pc) with
 * the 12-value enum below; pc = current_cpu ? regs[15] : 0 (aux/adi
 * pattern). Enable with --trace "sc6530_dsp_*" + -D. For REG_WRITE /
 * REG_READ / UNKNOWN the arg is PACKED: (addr_low16 << 16) | (val & 0xffff)
 * - the companion qemu_log line always carries the FULL 32-bit value and
 * the full guest address, so the Wave-6 diff loses nothing. In addition
 * every handshake transition logs a "sc6530_dsp: arm_ctl=0x.. dsp_ctl=0x.."
 * line (spec 6: "log every shared-mem interaction - (a) the arm_ctl word
 * the guest writes, (b) the dsp_ctl word the fake answers").
 *
 * The event enum (id value = index, matches sc6530_dsp_ev_names[]):
 *   SM_BOOT=0  reset released (spec 3a)        arg=(strap<<16)|boot_vector
 *   SM_READY=1 fake sets DSP_READY             arg=dsp_ctl word
 *   SM_BLOCK=2 guest writes dl_offset/size     arg=(offset<<16)|block_size
 *   SM_DATA=3  guest writes READY|DATA_READY   arg=arm_ctl word
 *   SM_COPY=4  fake sets COPY_DONE + IRQ       arg=arm_ctl at START_COPY
 *   SM_RUN=5   fake sets DSP_RUN               arg=dsp_ctl word
 *   REG_WRITE=6 any spec-2c APB register write arg=(addr_off<<16)|(val&0xffff)
 *   REG_READ=7 any spec-2c APB register read   arg=(addr_off<<16)|(val&0xffff)
 *   IRQ_ARM=8  INT_SET_CLR0 b0 (ARM->DSP IRQ)  arg=val
 *   CLK_FORCE=9 DSP_CTL0 b1 set/clear (spec 2d) arg=val
 *   OWNERSHIP=10 0x8b0001c4 b10/b9/b2 change (spec 5) arg=val
 *   UNKNOWN=11 unexpected/unanswered access    arg=(addr_off<<16)|(val&0xffff)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"
#include "hw/core/sysbus.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev.h"
#include "target/arm/cpu.h"
#include "system/memory.h"
#include "trace.h"

#define TYPE_SC6530_DSP "sc6530_dsp"
OBJECT_DECLARE_SIMPLE_TYPE(Sc6530DspState, SC6530_DSP)

/* ---------------------------------------------------------------------- */
/* Register geometry (spec 2c; GLB_REG_BASE = 0x8b000000)                 */
/* ---------------------------------------------------------------------- */

#define SC6530_DSP_INT_STS0_ADDR     0x8b000140ULL   /* DSP_IRQ = b2       */
#define SC6530_DSP_INT_SET_CLR0_ADDR 0x8b000160ULL   /* CLR b2/b3, SET b0  */
#define SC6530_DSP_RST0_SET_ADDR     0x8b001068ULL   /* DSP_SOFT_RST = b16 */
#define SC6530_DSP_RST0_CLR_ADDR     0x8b002068ULL   /* DSP_SOFT_RST = b16 */
#define SC6530_DSP_MCU_CTL0_ADDR     0x8b0001a0ULL   /* BOOT_ADDR, BOOT_EN */
#define SC6530_DSP_DSP_CTL0_ADDR     0x8b0001c0ULL   /* strap, CLK_FORCE   */
#define SC6530_DSP_PERI_CTL0_ADDR    0x8b0001c4ULL   /* audio ownership    */
/* SC6530C ground truth (Ghidra on dump_firmware.bin, 2026-08-27): the
 * SC6530C DSP soft reset is bit 16 of the APB_EB0 SET/CLR pair (the stock
 * downloader FUN_00067fc8 writes 0x10000 to 0x8b000060/0x64) — NOT the
 * SDK's APB_RST0 0x8b001068/0x2068 (0 literal hits in this dump). */
#define SC6530_DSP_APB_EB0_SET_ADDR  0x8b000060ULL
#define SC6530_DSP_APB_EB0_CLR_ADDR  0x8b000064ULL

/* Share-memory control windows: the 20-byte control struct (10 halfwords,
 * SHARE_MEM_CTRL_SIZE) at the base of BOTH candidate bases (spec 1/2a). */
#define SC6530_DSP_SHARE_CTL_BASE    0x10000000ULL
#define SC6530_DSP_SHARE_CTL_ALIAS   0x30000000ULL
#define SC6530_DSP_SHARE_CTL_SIZE    0x20
/* Wave-5: the stock OS's DSP download control struct lives at sharemem
 * base + 0xFE0 (runtime [0x0422E598] = 0x10000FE0), so the control
 * windows are mapped at base+0xFE0 and must forward to the RAM at that
 * offset (the base+0x00 windows never saw guest traffic). */
#define SC6530_DSP_SHARE_RAM_OFF     0xFE0
/* Wave-8 (dsp-boot, 2026-08-27): the DOWNLOAD handshake (arm_ctl/dsp_ctl/
 * dl_offset/dl_block_size, per block) runs at sharemem base + 0 — the
 * stock downloader FUN_0003aa76 sets the control base to 0x10000000 and
 * the boot ROM answers there. The fake now watches BOTH control areas:
 * +0x00 (download) and +0xFE0 (post-boot status). */

/* Control-struct halfword offsets (spec 2a). */
#define SC6530_DSP_SM_ARM_CTL_OFF    0   /* arm_control_status, guest->fake */
#define SC6530_DSP_SM_DSP_CTL_OFF    2   /* dsp_control_status, fake->guest */
#define SC6530_DSP_SM_DL_OFFSET_OFF  4   /* dl_offset, u16, guest (trace)   */
#define SC6530_DSP_SM_DL_SIZE_OFF    6   /* dl_block_size, u16, guest       */

/* Status bits (spec 2b). */
#define SC6530_DSP_ARM_READY        0x0001u
#define SC6530_DSP_ARM_DATA_READY   0x0002u
#define SC6530_DSP_ARM_START_COPY   0x0004u
#define SC6530_DSP_ARM_BOOT_DONE    0x0008u
#define SC6530_DSP_DSP_READY        0x0001u
#define SC6530_DSP_DSP_READY_TO_COPY 0x0002u
#define SC6530_DSP_DSP_COPY_DONE    0x0004u
#define SC6530_DSP_DSP_RUN          0x0008u

/* APB register bit fields (spec 2c). */
#define SC6530_DSP_RST_SOFT_BIT     (1u << 16)      /* RST0_SET/CLR b16    */
#define SC6530_DSP_MCU_BOOT_EN      (1u << 16)      /* MCU_CTL0 b16        */
#define SC6530_DSP_DSP_CLK_FORCE    (1u << 1)       /* DSP_CTL0 b1 (spec 2d)*/
#define SC6530_DSP_IRQ_MCU_SET      (1u << 0)       /* INT_SET_CLR0 b0     */
#define SC6530_DSP_IRQ_DSP_CLR      (1u << 2)       /* INT_SET_CLR0 b2     */
#define SC6530_DSP_IRQ_FRQ_CLR      (1u << 3)       /* INT_SET_CLR0 b3     */
#define SC6530_DSP_IRQ_DSP_BIT      (1u << 2)       /* INT_STS0 b2         */
#define SC6530_DSP_PERI_OWN_MASK    (0x400u | 0x200u | 0x4u)  /* b10|b9|b2 */

/* ---------------------------------------------------------------------- */
/* Trace-event id enum (spec 6; see the header comment for the arg        */
/* packing of REG_WRITE/REG_READ/UNKNOWN)                                 */
/* ---------------------------------------------------------------------- */

typedef enum {
    SC6530_DSP_EV_SM_BOOT = 0,
    SC6530_DSP_EV_SM_READY,
    SC6530_DSP_EV_SM_BLOCK,
    SC6530_DSP_EV_SM_DATA,
    SC6530_DSP_EV_SM_COPY,
    SC6530_DSP_EV_SM_RUN,
    SC6530_DSP_EV_REG_WRITE,
    SC6530_DSP_EV_REG_READ,
    SC6530_DSP_EV_IRQ_ARM,
    SC6530_DSP_EV_CLK_FORCE,
    SC6530_DSP_EV_OWNERSHIP,
    SC6530_DSP_EV_UNKNOWN,
    SC6530_DSP_EV_MAX
} Sc6530DspEvent;

static const char *const sc6530_dsp_ev_names[] = {
    "SM_BOOT", "SM_READY", "SM_BLOCK", "SM_DATA", "SM_COPY", "SM_RUN",
    "REG_WRITE", "REG_READ", "IRQ_ARM", "CLK_FORCE", "OWNERSHIP", "UNKNOWN",
};

/* Forward declarations: the window / handshake helpers are defined below
 * but referenced from the share-window and APB handlers above. */
static void sc6530_dsp_arm_ctl_update(Sc6530DspState *s, uint32_t ram_off);
static void sc6530_dsp_block_meta_update(Sc6530DspState *s, uint32_t ram_off);
static void sc6530_dsp_boot_release(Sc6530DspState *s);

/* ---------------------------------------------------------------------- */
/* Device state                                                           */
/* ---------------------------------------------------------------------- */

typedef enum {
    SC6530_DSP_REG_INT_STS0 = 0,
    SC6530_DSP_REG_INT_SET_CLR0,
    SC6530_DSP_REG_RST0_SET,
    SC6530_DSP_REG_RST0_CLR,
    SC6530_DSP_REG_MCU_CTL0,
    SC6530_DSP_REG_DSP_CTL0,
    SC6530_DSP_REG_PERI_CTL0,
    SC6530_DSP_REG_EB0_SET,
    SC6530_DSP_REG_EB0_CLR,
    SC6530_DSP_REG_COUNT
} Sc6530DspRegId;

static const hwaddr sc6530_dsp_reg_addrs[SC6530_DSP_REG_COUNT] = {
    SC6530_DSP_INT_STS0_ADDR, SC6530_DSP_INT_SET_CLR0_ADDR,
    SC6530_DSP_RST0_SET_ADDR, SC6530_DSP_RST0_CLR_ADDR,
    SC6530_DSP_MCU_CTL0_ADDR, SC6530_DSP_DSP_CTL0_ADDR,
    SC6530_DSP_PERI_CTL0_ADDR,
    SC6530_DSP_APB_EB0_SET_ADDR, SC6530_DSP_APB_EB0_CLR_ADDR,
};

/* Per-region opaque: which APB register an access belongs to. */
typedef struct {
    Sc6530DspState *s;
    Sc6530DspRegId reg;
} Sc6530DspRegCtx;

/* Per-window opaque: the share control window + the RAM offset it
 * forwards to (0 for the download handshake, 0xFE0 for the post-boot
 * status area). */
typedef struct {
    Sc6530DspState *s;
    uint32_t ram_off;
} Sc6530DspShareCtx;

struct Sc6530DspState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion apb_mr[SC6530_DSP_REG_COUNT];  /* 4-byte APB subregions */
    MemoryRegion share_mr[4];                   /* 0x20 control windows  */
    Sc6530DspRegCtx apb_ctx[SC6530_DSP_REG_COUNT];
    Sc6530DspShareCtx share_ctx[4];

    /* QOM link "sharemem": the b310e.c RAM region (NOT re-mapped here). */
    MemoryRegion *sharemem;
    uint8_t *sharemem_ptr;      /* resolved in realize (NULL if unlinked) */

    /* APB register storage (store+echo). */
    uint32_t apb_regs[SC6530_DSP_REG_COUNT];

    /* Handshake state machine (spec 3). */
    bool reset_held;            /* DSP in reset (RST0_SET b16) */
    uint16_t prev_arm_ctl;      /* edge-not-level tracking (spec 3e) */
    uint16_t dsp_ctl;           /* the fake's answer word (+2 in RAM) */
    bool dsp_irq_pending;       /* latched INT_STS0 b2 (spec 3b) */
    uint16_t dl_offset;         /* last-seen dl_offset (SM_BLOCK trace) */
    uint16_t dl_block_size;     /* last-seen dl_block_size */
};

/* ---------------------------------------------------------------------- */
/* Helpers                                                               */
/* ---------------------------------------------------------------------- */

static uint32_t sc6530_dsp_guest_pc(void)
{
    CPUState *cs = current_cpu;

    if (cs) {
        return ARM_CPU(cs)->env.regs[15];
    }
    return 0;
}

/* Emit the spec-6 trace event + the companion human-readable log line. */
static void sc6530_dsp_emit(Sc6530DspState *s, Sc6530DspEvent ev,
                            uint32_t arg)
{
    uint32_t pc = sc6530_dsp_guest_pc();

    trace_sc6530_dsp_cmd(ev, arg, pc);
    qemu_log("sc6530_dsp: %-10s arg=0x%08x pc=0x%08x\n",
             sc6530_dsp_ev_names[ev], arg, pc);
}

static uint32_t sc6530_dsp_apb_addr(Sc6530DspRegId reg)
{
    return (uint32_t)sc6530_dsp_reg_addrs[reg];
}

/* Size-aware store+echo on the 4-byte APB storage (aux/adi pattern). */
static uint64_t sc6530_dsp_reg_read_word(const uint32_t *word, hwaddr offset,
                                         unsigned size)
{
    return extract32(*word, (offset % 4) * 8, size * 8);
}

static void sc6530_dsp_reg_write_word(uint32_t *word, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
    unsigned shift = (offset % 4) * 8;

    *word = (*word & ~(mask << shift)) | ((uint32_t)value & mask) << shift;
}

/* Pack (addr_low16 << 16) | (val & 0xffff) for the REG_WRITE, REG_READ
 * and UNKNOWN event args; the qemu_log line always carries the full
 * 32-bit value. */
static uint32_t sc6530_dsp_pack_arg(Sc6530DspRegId reg, uint32_t val)
{
    return ((sc6530_dsp_apb_addr(reg) & 0xffff) << 16) | (val & 0xffff);
}

/* Reset the handshake state machine (spec 2c RST0_SET / 3a release).
 * The APB register storage is NOT cleared (store+echo semantics); only
 * the machine state is reset. The answer word is mirrored to the RAM
 * (a held/reset DSP answers nothing) in BOTH control areas (0 = download
 * handshake, 0xFE0 = post-boot status). */
static void sc6530_dsp_machine_reset(Sc6530DspState *s)
{
    s->prev_arm_ctl = 0;
    s->dsp_ctl = 0;
    s->dsp_irq_pending = false;
    s->dl_offset = 0;
    s->dl_block_size = 0;
    if (s->sharemem_ptr) {
        stw_le_p(s->sharemem_ptr + 0 + SC6530_DSP_SM_DSP_CTL_OFF, s->dsp_ctl);
        stw_le_p(s->sharemem_ptr + SC6530_DSP_SHARE_RAM_OFF + SC6530_DSP_SM_DSP_CTL_OFF,
                 s->dsp_ctl);
    }
}

/* ---------------------------------------------------------------------- */
/* Share-memory control window: forwards to the RAM (store/load honestly, */
/* spec 2a) and watches the control fields.                               */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_dsp_share_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    Sc6530DspShareCtx *ctx = opaque;
    Sc6530DspState *s = ctx->s;
    uint32_t word;

    if (!s->sharemem_ptr) {
        return 0;
    }
    word = ldl_le_p(s->sharemem_ptr + ctx->ram_off + (offset & ~3));
    return extract32(word, (offset & 3) * 8, size * 8);
}

static void sc6530_dsp_share_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    Sc6530DspShareCtx *ctx = opaque;
    Sc6530DspState *s = ctx->s;

    if (s->sharemem_ptr) {
        uint32_t word = ldl_le_p(s->sharemem_ptr + ctx->ram_off +
                                 (offset & ~3));
        uint32_t mask = (size == 4) ? 0xffffffffu : ((1u << (size * 8)) - 1);
        unsigned shift = (offset & 3) * 8;

        stl_le_p(s->sharemem_ptr + ctx->ram_off + (offset & ~3),
                 (word & ~(mask << shift)) | ((uint32_t)value & mask) << shift);
    }

    /* Watch the control fields (only when the write covers them). */
    if (offset < SC6530_DSP_SM_DSP_CTL_OFF && offset + size >
        SC6530_DSP_SM_ARM_CTL_OFF) {
        sc6530_dsp_arm_ctl_update(s, ctx->ram_off);
    }
    if (offset < SC6530_DSP_SM_DL_SIZE_OFF + 2 && offset + size >
        SC6530_DSP_SM_DL_OFFSET_OFF) {
        sc6530_dsp_block_meta_update(s, ctx->ram_off);
    }
}

static const MemoryRegionOps sc6530_dsp_share_ops = {
    .read  = sc6530_dsp_share_read,
    .write = sc6530_dsp_share_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* Guest wrote dl_offset / dl_block_size (spec 3b): trace SM_BLOCK when a
 * field changed (per block the pair is re-written with new values). */
static void sc6530_dsp_block_meta_update(Sc6530DspState *s, uint32_t ram_off)
{
    uint16_t off, size;

    if (!s->sharemem_ptr) {
        return;
    }
    off = lduw_le_p(s->sharemem_ptr + ram_off + SC6530_DSP_SM_DL_OFFSET_OFF);
    size = lduw_le_p(s->sharemem_ptr + ram_off + SC6530_DSP_SM_DL_SIZE_OFF);
    if (off == s->dl_offset && size == s->dl_block_size) {
        return;
    }
    s->dl_offset = off;
    s->dl_block_size = size;
    sc6530_dsp_emit(s, SC6530_DSP_EV_SM_BLOCK,
                    ((uint32_t)off << 16) | size);
}

/* The core handshake machine: edge-not-level on arm_control_status
 * (spec 3b/3c/3e). Runs on every arm_ctl write through either window.
 * `ram_off` identifies which control area the guest is using (0 = the
 * download handshake, 0xFE0 = the post-boot status area). */
static void sc6530_dsp_arm_ctl_update(Sc6530DspState *s, uint32_t ram_off)
{
    uint16_t new_ctl;
    uint32_t pc;

    if (!s->sharemem_ptr) {
        return;
    }
    new_ctl = lduw_le_p(s->sharemem_ptr + ram_off + SC6530_DSP_SM_ARM_CTL_OFF);
    if (new_ctl != s->prev_arm_ctl) {
        pc = sc6530_dsp_guest_pc();
        if (!s->reset_held) {
            /* DATA_READY 0->1 edge: answer READY_TO_COPY (spec 3b). */
            if ((new_ctl & SC6530_DSP_ARM_DATA_READY) &&
                !(s->prev_arm_ctl & SC6530_DSP_ARM_DATA_READY)) {
                s->dsp_ctl = SC6530_DSP_DSP_READY_TO_COPY;
                trace_sc6530_dsp_cmd(SC6530_DSP_EV_SM_DATA, new_ctl, pc);
                qemu_log("sc6530_dsp: SM_DATA     arm_ctl=0x%04x "
                         "dsp_ctl=0x%04x pc=0x%08x\n",
                         new_ctl, s->dsp_ctl, pc);
            }
            /* START_COPY 0->1 edge: COPY_DONE + latched DSP_IRQ; the last
             * block (BOOT_DONE set) also gets RUN (spec 3b/3c). */
            if ((new_ctl & SC6530_DSP_ARM_START_COPY) &&
                !(s->prev_arm_ctl & SC6530_DSP_ARM_START_COPY)) {
                s->dsp_ctl = SC6530_DSP_DSP_COPY_DONE;
                if (new_ctl & SC6530_DSP_ARM_BOOT_DONE) {
                    s->dsp_ctl |= SC6530_DSP_DSP_RUN;
                }
                s->dsp_irq_pending = true;
                trace_sc6530_dsp_cmd(SC6530_DSP_EV_SM_COPY, new_ctl, pc);
                qemu_log("sc6530_dsp: SM_COPY     arm_ctl=0x%04x "
                         "dsp_ctl=0x%04x irq=1 pc=0x%08x\n",
                         new_ctl, s->dsp_ctl, pc);
            }
            /* Tail rule (spec 3c): arm_ctl == BOOT_DONE alone and RUN not
             * yet set -> RUN. */
            if (new_ctl == SC6530_DSP_ARM_BOOT_DONE &&
                !(s->dsp_ctl & SC6530_DSP_DSP_RUN)) {
                s->dsp_ctl |= SC6530_DSP_DSP_RUN;
                trace_sc6530_dsp_cmd(SC6530_DSP_EV_SM_RUN, s->dsp_ctl, pc);
                qemu_log("sc6530_dsp: SM_RUN      dsp_ctl=0x%04x "
                         "pc=0x%08x\n", s->dsp_ctl, pc);
            }
        } else {
            qemu_log("sc6530_dsp: arm_ctl=0x%04x (DSP held in reset - "
                     "ignored) pc=0x%08x\n", new_ctl, pc);
        }
        s->prev_arm_ctl = new_ctl;
    }
    /* Wave-5: the stock OS's DSP status check polls [sharemem+4] == 1 as
     * its "DSP responded" readiness (dump 0x1a516 writes arm_ctl=3,
     * dl_offset=0, then checks the u16 at +4). Answer it permissively
     * (spec 4 UNKNOWN-5 rule) whenever the guest drives DATA_READY. */
    if (lduw_le_p(s->sharemem_ptr + ram_off + SC6530_DSP_SM_ARM_CTL_OFF) &
        SC6530_DSP_ARM_DATA_READY) {
        stw_le_p(s->sharemem_ptr + ram_off + SC6530_DSP_SM_DL_OFFSET_OFF, 1);
    }
    /* Always re-assert the fake's answer word: the fake owns +2 (a
     * 32-bit write to +0 could have clobbered it). */
    stw_le_p(s->sharemem_ptr + ram_off + SC6530_DSP_SM_DSP_CTL_OFF, s->dsp_ctl);
}

/* ---------------------------------------------------------------------- */
/* APB register handlers (spec 2c)                                        */
/* ---------------------------------------------------------------------- */

static uint64_t sc6530_dsp_apb_read(void *opaque, hwaddr offset,
                                    unsigned size)
{
    Sc6530DspRegCtx *ctx = opaque;
    Sc6530DspState *s = ctx->s;
    Sc6530DspRegId reg = ctx->reg;
    uint32_t val;
    uint32_t addr = sc6530_dsp_apb_addr(reg);

    switch (reg) {
    case SC6530_DSP_REG_INT_STS0:
        /* Spec 2c: report DSP_IRQ (b2) while the fake's IRQ is pending;
         * every other bit reads 0. */
        val = s->dsp_irq_pending ? SC6530_DSP_IRQ_DSP_BIT : 0;
        break;
    case SC6530_DSP_REG_RST0_SET:
    case SC6530_DSP_REG_RST0_CLR:
        /* Write-only registers: an unexpected read is UNKNOWN traffic
         * (spec 4: log, do not interpret). Answer the stored echo so a
         * read-modify-write chain stays stable. */
        val = s->apb_regs[reg];
        sc6530_dsp_emit(s, SC6530_DSP_EV_UNKNOWN,
                        sc6530_dsp_pack_arg(reg, val));
        break;
    default:
        val = s->apb_regs[reg];
        break;
    }
    sc6530_dsp_emit(s, SC6530_DSP_EV_REG_READ,
                    sc6530_dsp_pack_arg(reg, val));
    qemu_log("sc6530_dsp: read  reg=0x%08x val=0x%08x pc=0x%08x\n",
             addr, val, sc6530_dsp_guest_pc());
    return sc6530_dsp_reg_read_word(&val, offset, size);
}

static void sc6530_dsp_apb_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    Sc6530DspRegCtx *ctx = opaque;
    Sc6530DspState *s = ctx->s;
    Sc6530DspRegId reg = ctx->reg;
    uint32_t addr = sc6530_dsp_apb_addr(reg);
    uint32_t old = s->apb_regs[reg];

    sc6530_dsp_reg_write_word(&s->apb_regs[reg], offset, value, size);
    value = s->apb_regs[reg];

    switch (reg) {
    case SC6530_DSP_REG_INT_STS0:
        /* Read-only: an unexpected write is UNKNOWN traffic (log only;
         * the stored echo keeps RMW chains stable). */
        sc6530_dsp_emit(s, SC6530_DSP_EV_UNKNOWN,
                        sc6530_dsp_pack_arg(reg, value));
        break;
    case SC6530_DSP_REG_INT_SET_CLR0:
        /* Spec 2c: b2/b3 clear the pending DSP_IRQ; b0 = MCU_IRQ_SET
         * (ARM->DSP IRQ) is logged, not interpreted. */
        if (value & (SC6530_DSP_IRQ_DSP_CLR | SC6530_DSP_IRQ_FRQ_CLR)) {
            s->dsp_irq_pending = false;
            qemu_log("sc6530_dsp: DSP_IRQ cleared by INT_SET_CLR0=0x%08x "
                     "pc=0x%08x\n", (uint32_t)value, sc6530_dsp_guest_pc());
        }
        if (value & SC6530_DSP_IRQ_MCU_SET) {
            sc6530_dsp_emit(s, SC6530_DSP_EV_IRQ_ARM, value);
        }
        break;
    case SC6530_DSP_REG_RST0_SET:
        /* Spec 2c: b16 holds the DSP in reset and resets the handshake
         * state machine (spec 3d teardown uses it too). */
        if (value & SC6530_DSP_RST_SOFT_BIT) {
            s->reset_held = true;
            sc6530_dsp_machine_reset(s);
            qemu_log("sc6530_dsp: DSP held in reset (RST0_SET b16) "
                     "pc=0x%08x\n", sc6530_dsp_guest_pc());
        }
        break;
    case SC6530_DSP_REG_RST0_CLR:
        /* Spec 3a: b16 releases reset -> boot sequence: reset the machine,
         * answer DSP_READY immediately (permissive, UNKNOWN-5 timing). */
        if (value & SC6530_DSP_RST_SOFT_BIT) {
            sc6530_dsp_boot_release(s);
        }
        break;
    case SC6530_DSP_REG_EB0_SET:
        /* SC6530C (Ghidra-verified): bit 16 of the APB_EB0 SET pair is the
         * DSP soft-reset HOLD (the stock downloader FUN_00067fc8) — the
         * SDK's RST0 0x8b001068/0x2068 have 0 literal hits in the dump. */
        if (value & SC6530_DSP_RST_SOFT_BIT) {
            s->reset_held = true;
            sc6530_dsp_machine_reset(s);
            qemu_log("sc6530_dsp: DSP held in reset (APB_EB0_SET b16) "
                     "pc=0x%08x\n", sc6530_dsp_guest_pc());
        }
        break;
    case SC6530_DSP_REG_EB0_CLR:
        /* SC6530C release — mirrors RST0_CLR. */
        if (value & SC6530_DSP_RST_SOFT_BIT) {
            sc6530_dsp_boot_release(s);
        }
        break;
    case SC6530_DSP_REG_MCU_CTL0:
        /* store+echo; BOOT_ADDR[15:0] + BOOT_EN b16 are latched in the
         * storage (read at release time, spec 3a). */
        break;
    case SC6530_DSP_REG_DSP_CTL0:
        /* store+echo; spec 2d: CLK_FORCE b1 set/clear is traced. */
        if ((old ^ value) & SC6530_DSP_DSP_CLK_FORCE) {
            sc6530_dsp_emit(s, SC6530_DSP_EV_CLK_FORCE, value);
        }
        break;
    case SC6530_DSP_REG_PERI_CTL0:
        /* Spec 5: the DSP device OWNS this register. A 0x600 write is an
         * ARM-side ownership assertion, NOT a DSP command - never treat
         * it as traffic. OWNERSHIP traces b10/b9/b2 changes only. */
        if ((old ^ value) & SC6530_DSP_PERI_OWN_MASK) {
            sc6530_dsp_emit(s, SC6530_DSP_EV_OWNERSHIP, value);
        }
        break;
    default:
        break;
    }

    sc6530_dsp_emit(s, SC6530_DSP_EV_REG_WRITE,
                    sc6530_dsp_pack_arg(reg, value));
    qemu_log("sc6530_dsp: write reg=0x%08x val=0x%08x pc=0x%08x\n",
             addr, (uint32_t)value, sc6530_dsp_guest_pc());
}

static const MemoryRegionOps sc6530_dsp_apb_ops = {
    .read  = sc6530_dsp_apb_read,
    .write = sc6530_dsp_apb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* Boot release (spec 3a): reset the machine, then answer the PROTOCOL_3
 * DSP_READY poll with dsp_ctl = DSP_READY immediately. If BOOT_EN/strap
 * are absent at release, answer permissively anyway and log the anomaly. */
static void sc6530_dsp_boot_release(Sc6530DspState *s)
{
    uint32_t boot_vector = s->apb_regs[SC6530_DSP_REG_MCU_CTL0] & 0xffff;
    uint32_t strap = (s->apb_regs[SC6530_DSP_REG_DSP_CTL0] >> 3) & 0x1f;
    bool boot_en = (s->apb_regs[SC6530_DSP_REG_MCU_CTL0] >> 16) & 1;

    s->reset_held = false;
    sc6530_dsp_machine_reset(s);

    if (!boot_en || strap == 0) {
        qemu_log("sc6530_dsp: boot released without BOOT_EN=%u/strap=0x%x "
                 "(anomaly - answering permissively anyway) pc=0x%08x\n",
                 boot_en, strap, sc6530_dsp_guest_pc());
    }
    sc6530_dsp_emit(s, SC6530_DSP_EV_SM_BOOT,
                    (strap << 16) | boot_vector);
    s->dsp_ctl = SC6530_DSP_DSP_READY;
    if (s->sharemem_ptr) {
        stw_le_p(s->sharemem_ptr + 0 + SC6530_DSP_SM_DSP_CTL_OFF, s->dsp_ctl);
        stw_le_p(s->sharemem_ptr + SC6530_DSP_SHARE_RAM_OFF + SC6530_DSP_SM_DSP_CTL_OFF,
                 s->dsp_ctl);
    }
    sc6530_dsp_emit(s, SC6530_DSP_EV_SM_READY, s->dsp_ctl);
    qemu_log("sc6530_dsp: boot released: dsp_ctl=0x%04x (DSP_READY) "
             "pc=0x%08x\n", s->dsp_ctl, sc6530_dsp_guest_pc());
}

/* ---------------------------------------------------------------------- */
/* SysBus device                                                          */
/* ---------------------------------------------------------------------- */

static void sc6530_dsp_realize(DeviceState *dev, Error **errp)
{
    Sc6530DspState *s = SC6530_DSP(dev);

    if (!s->sharemem || !memory_region_is_ram(s->sharemem)) {
        warn_report("sc6530_dsp: 'sharemem' link missing or not a RAM "
                    "region - handshake state machine disabled (shared-mem "
                    "traffic degrades to logged no-ops)");
        return;
    }
    s->sharemem_ptr = memory_region_get_ram_ptr(s->sharemem);
}

static void sc6530_dsp_reset(DeviceState *dev)
{
    Sc6530DspState *s = SC6530_DSP(dev);

    memset(s->apb_regs, 0, sizeof(s->apb_regs));
    s->reset_held = true;       /* the DSP starts held in reset */
    sc6530_dsp_machine_reset(s);
}

static void sc6530_dsp_init(Object *obj)
{
    Sc6530DspState *s = SC6530_DSP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    char name[32];
    int i;

    /* 7 four-byte APB subregions (spec 2c), each with its own context so
     * the handlers know which register the access is for. */
    for (i = 0; i < SC6530_DSP_REG_COUNT; i++) {
        s->apb_ctx[i].s = s;
        s->apb_ctx[i].reg = i;
        snprintf(name, sizeof(name), "sc6530-dsp-apb-%d", i);
        memory_region_init_io(&s->apb_mr[i], obj, &sc6530_dsp_apb_ops,
                              &s->apb_ctx[i], name, 4);
        sysbus_init_mmio(sbd, &s->apb_mr[i]);
    }

    /* 4 control windows: at sharemem base + 0 (the DOWNLOAD handshake —
     * the stock downloader FUN_0003aa76 uses [0x10000000] directly, 2026-08-27
     * Ghidra-verified) and at base + 0xFE0 (Wave-5 finding: the post-boot
     * status area, the stock's runtime [0x0422E598] = 0x10000FE0), each at
     * the real base and its 0x30000000 defensive alias. The windows forward
     * to the RAM at their ram_off and watch the handshake fields. */
    s->share_ctx[0].s = s; s->share_ctx[0].ram_off = 0;
    s->share_ctx[1].s = s; s->share_ctx[1].ram_off = 0;
    s->share_ctx[2].s = s; s->share_ctx[2].ram_off = SC6530_DSP_SHARE_RAM_OFF;
    s->share_ctx[3].s = s; s->share_ctx[3].ram_off = SC6530_DSP_SHARE_RAM_OFF;
    for (i = 0; i < 4; i++) {
        snprintf(name, sizeof(name), "sc6530-dsp-share-ctl-%d", i);
        memory_region_init_io(&s->share_mr[i], obj, &sc6530_dsp_share_ops,
                              &s->share_ctx[i], name,
                              SC6530_DSP_SHARE_CTL_SIZE);
        sysbus_init_mmio(sbd, &s->share_mr[i]);
    }

    /* QOM link to the b310e.c sharemem RAM region (v11.1.0 six-arg form;
     * the keypad 'adi' link is the in-tree precedent). Set by b310e.c
     * BEFORE realize; resolved to a RAM pointer in realize. */
    object_property_add_link(obj, "sharemem", TYPE_MEMORY_REGION,
                             (Object **)&s->sharemem,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
}

static void sc6530_dsp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Spreadtrum SC6530 DSP fake (share-mem handshake + APB)";
    dc->realize = sc6530_dsp_realize;
    device_class_set_legacy_reset(dc, sc6530_dsp_reset);
}

static const TypeInfo sc6530_dsp_info = {
    .name          = TYPE_SC6530_DSP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Sc6530DspState),
    .instance_init = sc6530_dsp_init,
    .class_init    = sc6530_dsp_class_init,
};

static void sc6530_dsp_register_types(void)
{
    type_register_static(&sc6530_dsp_info);
}

type_init(sc6530_dsp_register_types)
